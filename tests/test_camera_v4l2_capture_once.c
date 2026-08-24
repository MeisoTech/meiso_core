#include "camera_v4l2_adapter.h"

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

enum { FAKE_FD = 73 };
#define FAKE_MAPPING_BYTES 118784U

#ifdef MEISO_K230_LINUX_TARGET
#define EXPECTED_BUILD_TARGET "k230d_linux_candidate"
#else
#define EXPECTED_BUILD_TARGET "native_host"
#endif

typedef struct {
    uint8_t *storage;
    size_t storage_size;
    int open_error;
    unsigned open_calls;
    unsigned close_calls;
    unsigned map_calls;
    unsigned unmap_calls;
    unsigned queue_calls;
    unsigned dequeue_calls;
    unsigned delivered_frames;
    unsigned stream_on_calls;
    unsigned stream_off_calls;
    unsigned request_four_calls;
    unsigned request_zero_calls;
    bool would_block_once;
    bool bad_dequeue_field;
    uint32_t unmap_error_index;
    int unmap_error;
    bool unmap_error_once;
    int opened_flags;
    char opened_path[32];
} FakeCamera;

typedef struct {
    int status;
    char output[2048];
    size_t output_size;
    char error[512];
    size_t error_size;
} CapturedRun;

int meiso_camera_v4l2_capture_once_run(
    FILE *output,
    FILE *error_output,
    const MeisoCameraV4l2Config *config
);
int meiso_camera_v4l2_capture_once_test_write_max_receipt(FILE *output);

static int fake_open(
    void *context,
    const char *path,
    int flags,
    int *out_fd
)
{
    FakeCamera *camera = (FakeCamera *)context;

    ++camera->open_calls;
    camera->opened_flags = flags;
    (void)snprintf(camera->opened_path, sizeof(camera->opened_path), "%s", path);
    if (camera->open_error != 0) {
        return camera->open_error;
    }
    *out_fd = FAKE_FD;
    return 0;
}

static int fake_ioctl(
    void *context,
    int fd,
    MeisoCameraV4l2IoctlRequest request,
    MeisoCameraV4l2IoctlArgument *argument
)
{
    FakeCamera *camera = (FakeCamera *)context;

    if (fd != FAKE_FD || argument == NULL) {
        return EINVAL;
    }
    switch (request) {
        case MEISO_CAMERA_V4L2_IOCTL_QUERY_CAPABILITY:
            argument->capability.video_capture = true;
            argument->capability.streaming = true;
            break;
        case MEISO_CAMERA_V4L2_IOCTL_SET_FORMAT:
            if (argument->format.width != MEISO_CAMERA_V4L2_WIDTH ||
                argument->format.height != MEISO_CAMERA_V4L2_HEIGHT ||
                argument->format.pixel_format !=
                    MEISO_COMPOSITION_MEDIA_PIXEL_NV12 ||
                !argument->format.progressive ||
                argument->format.bytes_per_line !=
                    MEISO_CAMERA_V4L2_BYTES_PER_LINE ||
                argument->format.size_image != MEISO_CAMERA_V4L2_SIZE_IMAGE) {
                return EINVAL;
            }
            break;
        case MEISO_CAMERA_V4L2_IOCTL_GET_FORMAT:
            argument->format.width = MEISO_CAMERA_V4L2_WIDTH;
            argument->format.height = MEISO_CAMERA_V4L2_HEIGHT;
            argument->format.pixel_format =
                MEISO_COMPOSITION_MEDIA_PIXEL_NV12;
            argument->format.progressive = true;
            argument->format.bytes_per_line =
                MEISO_CAMERA_V4L2_BYTES_PER_LINE;
            argument->format.size_image = MEISO_CAMERA_V4L2_SIZE_IMAGE;
            break;
        case MEISO_CAMERA_V4L2_IOCTL_REQUEST_BUFFERS:
            if (argument->request_buffers.count == 0U) {
                ++camera->request_zero_calls;
            } else if (argument->request_buffers.count ==
                       MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT) {
                ++camera->request_four_calls;
            } else {
                return EINVAL;
            }
            break;
        case MEISO_CAMERA_V4L2_IOCTL_QUERY_BUFFER:
            if (argument->buffer.index >=
                MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT) {
                return EINVAL;
            }
            argument->buffer.memory_offset =
                (uint64_t)argument->buffer.index * FAKE_MAPPING_BYTES;
            argument->buffer.length = FAKE_MAPPING_BYTES;
            break;
        case MEISO_CAMERA_V4L2_IOCTL_QUEUE_BUFFER:
            if (argument->buffer.index >=
                MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT) {
                return EINVAL;
            }
            ++camera->queue_calls;
            break;
        case MEISO_CAMERA_V4L2_IOCTL_STREAM_ON:
            ++camera->stream_on_calls;
            break;
        case MEISO_CAMERA_V4L2_IOCTL_DEQUEUE_BUFFER:
            ++camera->dequeue_calls;
            if (camera->would_block_once) {
                camera->would_block_once = false;
                return EAGAIN;
            }
            argument->buffer.index = camera->delivered_frames %
                MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT;
            argument->buffer.length = MEISO_CAMERA_V4L2_SIZE_IMAGE;
            argument->buffer.bytes_used = MEISO_CAMERA_V4L2_SIZE_IMAGE;
            argument->buffer.driver_sequence =
                9U + camera->delivered_frames;
            argument->buffer.driver_sequence_present = true;
            argument->buffer.timestamp_seconds = 12;
            argument->buffer.timestamp_microseconds =
                345678 + (int32_t)camera->delivered_frames;
            argument->buffer.timestamp_monotonic = true;
            argument->buffer.raw_field =
                camera->bad_dequeue_field ? UINT32_C(4) :
                    MEISO_CAMERA_V4L2_RAW_FIELD_NONE;
            argument->buffer.field_provenance =
                camera->bad_dequeue_field ?
                    MEISO_CAMERA_V4L2_FIELD_UNKNOWN :
                    MEISO_CAMERA_V4L2_FIELD_EXPLICIT_PROGRESSIVE;
            argument->buffer.frame_error = false;
            ++camera->delivered_frames;
            break;
        case MEISO_CAMERA_V4L2_IOCTL_STREAM_OFF:
            ++camera->stream_off_calls;
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
    FakeCamera *camera = (FakeCamera *)context;
    const uint64_t index = offset / FAKE_MAPPING_BYTES;

    if (fd != FAKE_FD || length != FAKE_MAPPING_BYTES ||
        index >= MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT ||
        offset != index * FAKE_MAPPING_BYTES ||
        out_mapping == NULL) {
        return EINVAL;
    }
    ++camera->map_calls;
    *out_mapping = camera->storage + (size_t)index * FAKE_MAPPING_BYTES;
    return 0;
}

static int fake_unmap(void *context, void *mapping, size_t length)
{
    FakeCamera *camera = (FakeCamera *)context;
    const uintptr_t value = (uintptr_t)mapping;
    const uintptr_t base = (uintptr_t)camera->storage;

    if (length != FAKE_MAPPING_BYTES || value < base ||
        value - base >= camera->storage_size ||
        (value - base) % FAKE_MAPPING_BYTES != 0U) {
        return EINVAL;
    }
    ++camera->unmap_calls;
    if ((uint32_t)((value - base) / FAKE_MAPPING_BYTES) ==
            camera->unmap_error_index && camera->unmap_error != 0) {
        const int result = camera->unmap_error;
        if (camera->unmap_error_once) {
            camera->unmap_error = 0;
        }
        return result;
    }
    return 0;
}

static int fake_close(void *context, int fd)
{
    FakeCamera *camera = (FakeCamera *)context;

    if (fd != FAKE_FD) {
        return EINVAL;
    }
    ++camera->close_calls;
    return 0;
}

static bool read_stream(
    FILE *stream,
    char *output,
    size_t capacity,
    size_t *out_size
)
{
    size_t size;

    if (stream == NULL || output == NULL || capacity == 0U ||
        out_size == NULL || fflush(stream) != 0 ||
        fseek(stream, 0L, SEEK_SET) != 0) {
        return false;
    }
    size = fread(output, 1U, capacity - 1U, stream);
    if (ferror(stream) != 0 || !feof(stream)) {
        return false;
    }
    output[size] = '\0';
    *out_size = size;
    return true;
}

static bool capture_run(
    const MeisoCameraV4l2Config *config,
    CapturedRun *captured
)
{
    FILE *output = NULL;
    FILE *error_output = NULL;
    bool okay = false;

    if (config == NULL || captured == NULL) {
        return false;
    }
    memset(captured, 0, sizeof(*captured));
    output = tmpfile();
    error_output = tmpfile();
    if (output == NULL || error_output == NULL) {
        goto cleanup;
    }
    captured->status = meiso_camera_v4l2_capture_once_run(
        output, error_output, config
    );
    if (!read_stream(
            output, captured->output, sizeof(captured->output),
            &captured->output_size
        ) ||
        !read_stream(
            error_output, captured->error, sizeof(captured->error),
            &captured->error_size
        )) {
        goto cleanup;
    }
    okay = true;

cleanup:
    if (error_output != NULL) {
        (void)fclose(error_output);
    }
    if (output != NULL) {
        (void)fclose(output);
    }
    return okay;
}

static void reset_calls(FakeCamera *camera)
{
    camera->open_calls = 0U;
    camera->close_calls = 0U;
    camera->map_calls = 0U;
    camera->unmap_calls = 0U;
    camera->queue_calls = 0U;
    camera->dequeue_calls = 0U;
    camera->delivered_frames = 0U;
    camera->stream_on_calls = 0U;
    camera->stream_off_calls = 0U;
    camera->request_four_calls = 0U;
    camera->request_zero_calls = 0U;
    camera->opened_flags = 0;
    memset(camera->opened_path, 0, sizeof(camera->opened_path));
}

static bool test_custom_capture_receipt(void)
{
    static const char expected[] =
        "{\"type\":\"camera_capture_probe\","
        "\"schema\":\"meiso.camera_v4l2_capture_probe.v1\","
        "\"verification\":\"THREE_FRAME_CAPTURE_RECEIPT\","
        "\"build_target\":\"" EXPECTED_BUILD_TARGET
        "\",\"pixel_format\":\"NV12\","
        "\"width\":320,\"height\":240,\"bytes_per_line\":320,"
        "\"size_image\":115200,\"driver_buffer_count\":4,"
        "\"mapped_buffer_count\":4,\"mapped_bytes\":475136,"
        "\"queued_buffers_running\":4,\"frame_count\":3,"
        "\"final_phase\":\"STOPPED\",\"final_mapped_count\":0,"
        "\"final_queued_count\":0,\"frames\":["
        "{\"ordinal\":1,\"source_clock_epoch\":91,"
        "\"monotonic_ns\":12345678000,\"raw_field\":1,"
        "\"field_provenance\":1,\"raw_driver_sequence\":9,"
        "\"driver_sequence_present\":true,\"effective_sequence\":9,"
        "\"sequence_provenance\":1,\"producer_dropped_frames\":0,"
        "\"bytes_used\":115200,\"fnv1a64\":\"7b90bf5253251f25\","
        "\"sha256\":\"781ee4ecb0ebaef3a8327af0de6e2f48b3ff27ea375a1a68a0a222b7efae80a8\"},"
        "{\"ordinal\":2,\"source_clock_epoch\":91,"
        "\"monotonic_ns\":12345679000,\"raw_field\":1,"
        "\"field_provenance\":1,\"raw_driver_sequence\":10,"
        "\"driver_sequence_present\":true,\"effective_sequence\":10,"
        "\"sequence_provenance\":1,\"producer_dropped_frames\":0,"
        "\"bytes_used\":115200,\"fnv1a64\":\"7b90bf5253251f25\","
        "\"sha256\":\"781ee4ecb0ebaef3a8327af0de6e2f48b3ff27ea375a1a68a0a222b7efae80a8\"},"
        "{\"ordinal\":3,\"source_clock_epoch\":91,"
        "\"monotonic_ns\":12345680000,\"raw_field\":1,"
        "\"field_provenance\":1,\"raw_driver_sequence\":11,"
        "\"driver_sequence_present\":true,\"effective_sequence\":11,"
        "\"sequence_provenance\":1,\"producer_dropped_frames\":0,"
        "\"bytes_used\":115200,\"fnv1a64\":\"7b90bf5253251f25\","
        "\"sha256\":\"781ee4ecb0ebaef3a8327af0de6e2f48b3ff27ea375a1a68a0a222b7efae80a8\"}]}\n";
    const MeisoCameraV4l2Syscalls syscalls = {
        fake_open, fake_ioctl, fake_map, fake_unmap, fake_close
    };
    FakeCamera camera;
    MeisoCameraV4l2Config config;
    CapturedRun first;
    CapturedRun second;
    size_t index;

    memset(&camera, 0, sizeof(camera));
    camera.storage_size =
        (size_t)MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT *
        FAKE_MAPPING_BYTES;
    camera.storage = (uint8_t *)malloc(camera.storage_size);
    CHECK(camera.storage != NULL);
    for (index = 0U; index < camera.storage_size; ++index) {
        camera.storage[index] = (uint8_t)(index * 37U + 11U);
    }
    config.device_path = "/dev/fake-camera";
    config.source_clock_epoch = 91U;
    config.syscalls = &syscalls;
    config.syscall_context = &camera;
    camera.would_block_once = true;
    CHECK(capture_run(&config, &first));
    CHECK(first.status == 0 && first.error_size == 0U &&
          strcmp(first.output, expected) == 0 &&
          first.output_size == sizeof(expected) - 1U &&
          camera.open_calls == 1U && camera.close_calls == 1U &&
          camera.map_calls == 4U && camera.unmap_calls == 4U &&
          camera.queue_calls == 7U && camera.dequeue_calls == 4U &&
          camera.stream_on_calls == 1U &&
          camera.stream_off_calls == 1U &&
          camera.request_four_calls == 1U &&
          camera.request_zero_calls == 1U &&
          camera.opened_flags == (O_RDWR | O_NONBLOCK | O_CLOEXEC) &&
          strcmp(camera.opened_path, config.device_path) == 0);
    reset_calls(&camera);
    camera.would_block_once = true;
    CHECK(capture_run(&config, &second) && second.status == 0 &&
          second.error_size == 0U &&
          second.output_size == first.output_size &&
          memcmp(second.output, first.output, first.output_size + 1U) == 0 &&
          camera.close_calls == 1U && camera.dequeue_calls == 4U);
    free(camera.storage);
    return true;
}

static bool test_error_is_stderr_only(void)
{
    const MeisoCameraV4l2Syscalls syscalls = {
        fake_open, fake_ioctl, fake_map, fake_unmap, fake_close
    };
    FakeCamera camera;
    MeisoCameraV4l2Config config;
    CapturedRun captured;

    memset(&camera, 0, sizeof(camera));
    camera.open_error = EACCES;
    config.device_path = "/dev/not-reported";
    config.source_clock_epoch = 91U;
    config.syscalls = &syscalls;
    config.syscall_context = &camera;
    CHECK(capture_run(&config, &captured) && captured.status == 3 &&
          captured.output_size == 0U &&
          strcmp(captured.error,
                 "{\"type\":\"camera_capture_error\","
                 "\"code\":\"E_START\",\"detail\":2}\n") == 0 &&
          strstr(captured.error, config.device_path) == NULL &&
          camera.open_calls == 1U && camera.close_calls == 0U);
    return true;
}

static bool test_poll_diagnostic_is_bounded_and_path_free(void)
{
    const MeisoCameraV4l2Syscalls syscalls = {
        fake_open, fake_ioctl, fake_map, fake_unmap, fake_close
    };
    FakeCamera camera;
    MeisoCameraV4l2Config config;
    CapturedRun captured;
    bool matches;

    memset(&camera, 0, sizeof(camera));
    camera.storage_size =
        (size_t)MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT *
        FAKE_MAPPING_BYTES;
    camera.storage = (uint8_t *)malloc(camera.storage_size);
    CHECK(camera.storage != NULL);
    memset(camera.storage, 0x5a, camera.storage_size);
    camera.bad_dequeue_field = true;
    config.device_path = "/dev/private-camera-path";
    config.source_clock_epoch = 91U;
    config.syscalls = &syscalls;
    config.syscall_context = &camera;
    matches = capture_run(&config, &captured) && captured.status == 5 &&
          captured.output_size == 0U &&
          strstr(captured.error,
                 "\"code\":\"E_POLL\",\"detail\":6") != NULL &&
          strstr(captured.error,
                 "\"primary_failure_stage\":11") != NULL &&
          strstr(captured.error,
                 "\"cleanup_failure_stage\":0") != NULL &&
          strstr(captured.error, "\"reject_reason\":5") != NULL &&
          strstr(captured.error, "\"raw_field\":4") != NULL &&
          strstr(captured.error,
                 "\"driver_sequence_present\":true") != NULL &&
          strstr(captured.error, config.device_path) == NULL &&
          strstr(captured.error,
                 "\"code\":\"E_CLEANUP\",\"detail\":1") != NULL;
    free(camera.storage);
    CHECK(matches);
    return true;
}

static bool test_cleanup_debt_is_bounded_and_explicit(void)
{
    const MeisoCameraV4l2Syscalls syscalls = {
        fake_open, fake_ioctl, fake_map, fake_unmap, fake_close
    };
    FakeCamera camera;
    MeisoCameraV4l2Config config;
    CapturedRun captured;
    size_t index;

    memset(&camera, 0, sizeof(camera));
    camera.storage_size =
        (size_t)MEISO_CAMERA_V4L2_REQUIRED_BUFFER_COUNT *
        FAKE_MAPPING_BYTES;
    camera.storage = (uint8_t *)malloc(camera.storage_size);
    CHECK(camera.storage != NULL);
    for (index = 0U; index < camera.storage_size; ++index) {
        camera.storage[index] = (uint8_t)(index * 37U + 11U);
    }
    config.device_path = "/dev/fake-camera";
    config.source_clock_epoch = 91U;
    config.syscalls = &syscalls;
    config.syscall_context = &camera;

    camera.unmap_error_index = 3U;
    camera.unmap_error = EIO;
    camera.unmap_error_once = true;
    CHECK(capture_run(&config, &captured) && captured.status == 5 &&
          captured.output_size == 0U &&
          strcmp(captured.error,
                 "{\"type\":\"camera_capture_error\","
                 "\"code\":\"E_CLEANUP\",\"detail\":1}\n") == 0 &&
          camera.stream_off_calls == 1U && camera.unmap_calls == 5U &&
          camera.request_zero_calls == 1U && camera.close_calls == 1U);

    reset_calls(&camera);
    camera.unmap_error_index = 3U;
    camera.unmap_error = EIO;
    camera.unmap_error_once = false;
    CHECK(capture_run(&config, &captured) && captured.status == 5 &&
          captured.output_size == 0U &&
          strcmp(captured.error,
                 "{\"type\":\"camera_capture_error\","
                 "\"code\":\"E_CLEANUP\",\"detail\":2}\n") == 0 &&
          camera.stream_off_calls == 1U && camera.unmap_calls == 5U &&
          camera.request_zero_calls == 1U && camera.close_calls == 1U);
    free(camera.storage);
    return true;
}

static bool test_max_receipt_is_one_complete_line(void)
{
    FILE *output = tmpfile();
    char line[2048];
    size_t size;
    size_t newline_count = 0U;
    size_t index;

    CHECK(output != NULL &&
          meiso_camera_v4l2_capture_once_test_write_max_receipt(output) == 0 &&
          read_stream(output, line, sizeof(line), &size) && size != 0U &&
          line[size - 1U] == '\n');
    for (index = 0U; index < size; ++index) {
        if (line[index] == '\n') {
            ++newline_count;
        }
    }
    CHECK(newline_count == 1U &&
          strstr(line, "\"mapped_bytes\":18446744073709551615") != NULL &&
          strstr(line,
                 "\"source_clock_epoch\":18446744073709551615") != NULL &&
          strstr(line, "\"monotonic_ns\":18446744073709551615") != NULL &&
          strstr(line, "\"raw_driver_sequence\":4294967295") != NULL &&
          strstr(line, "\"effective_sequence\":4294967295") != NULL &&
          strstr(line,
                 "\"producer_dropped_frames\":18446744073709551615") !=
              NULL);
    CHECK(fclose(output) == 0);
    return true;
}

int main(void)
{
    if (!test_custom_capture_receipt() || !test_error_is_stderr_only() ||
        !test_poll_diagnostic_is_bounded_and_path_free() ||
        !test_cleanup_debt_is_bounded_and_explicit()) {
        return 1;
    }
    if (!test_max_receipt_is_one_complete_line()) {
        return 1;
    }
    (void)puts("camera v4l2 capture-once tests passed");
    return 0;
}
