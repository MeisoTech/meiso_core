#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "adapters/meiso_drm_libdrm_single_plane_test_only_private.h"
#include "adapters/meiso_drm_single_plane_test_only_supervisor_private.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    JSON_CAPACITY = 16384,
    MAX_HASHED_FILE_BYTES = 64 * 1024 * 1024,
    MAX_RAW_OUTPUT_BYTES = 64 * 1024,
    GATE_POLL_NS = 100000000,
    CHILD_SETUP_FAILURE_EXIT = 1
};

_Static_assert(sizeof(pid_t) == sizeof(int32_t),
               "K230D Linux pid_t must be signed 32-bit");
_Static_assert((pid_t)-1 < (pid_t)0,
               "K230D Linux pid_t must be signed");

static const char PRIMARY_NODE[] = "/dev/dri/card0";
static const char GLOBAL_DIRECTORY[] = "/run";
static const char GLOBAL_RECORD[] =
    "meiso-k230d-drm-single-plane-test-only.launch.consumed.v1.json";
static const char EVIDENCE_RECORD[] = "launch.consumed.v1.json";
static const char READY_RECORD[] = "run.ready.v1.json";
static const char QUARANTINE_RECORD[] = "run.quarantine.v1.json";
static const char TERMINAL_RECORD[] = "run.result.v1.json";
static const char STDOUT_RECORD[] = "runner.stdout";
static const char STDERR_RECORD[] = "runner.stderr";

typedef struct {
    char bytes[JSON_CAPACITY];
    size_t size;
    bool valid;
} JsonBuffer;

typedef struct {
    int run_directory_fd;
    int global_directory_fd;
    int stdout_fd;
    int stderr_fd;
    int ready_read_fd;
    int ready_write_fd;
    pid_t supervisor_pid;
    pid_t child_pid;
    dev_t primary_device;
    bool primary_device_known;
    bool ready_byte_seen;
    char nonce_hex[MEISO_K230D_DRM_SUPERVISOR_NONCE_BYTES * 2U + 1U];
    MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding binding;
    sigset_t supervised_signals;
} LinuxSupervisorContext;

typedef struct {
    LinuxSupervisorContext *supervisor;
} LinuxChildContext;

static int positive_error(void)
{
    return errno > 0 ? errno : EIO;
}

static bool bytes_all_zero(const uint8_t *bytes, size_t size)
{
    size_t index;

    if (bytes == NULL) {
        return true;
    }
    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static void encode_hex(const uint8_t *bytes, size_t size, char *output)
{
    static const char HEX[] = "0123456789abcdef";
    size_t index;

    for (index = 0U; index < size; ++index) {
        output[index * 2U] = HEX[bytes[index] >> 4U];
        output[index * 2U + 1U] = HEX[bytes[index] & UINT8_C(0x0f)];
    }
    output[size * 2U] = '\0';
}

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

static bool decode_lower_hex_32(const char *text, uint8_t output[32])
{
    size_t index;

    if (text == NULL || strlen(text) != 64U) {
        return false;
    }
    for (index = 0U; index < 32U; ++index) {
        int high = hex_value(text[index * 2U]);
        int low = hex_value(text[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[index] = (uint8_t)((unsigned int)high << 4U) |
            (uint8_t)low;
    }
    return !bytes_all_zero(output, 32U);
}

static bool parse_canonical_u32(const char *text, uint32_t *output)
{
    uint64_t value = 0U;
    size_t index;

    if (text == NULL || output == NULL || text[0] == '\0' ||
        (text[0] == '0' && text[1] != '\0')) {
        return false;
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        unsigned int digit;
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
        digit = (unsigned int)(text[index] - '0');
        if (value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    if (value == 0U) {
        return false;
    }
    *output = (uint32_t)value;
    return true;
}

static void json_init(JsonBuffer *buffer)
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->valid = true;
}

static void json_append(JsonBuffer *buffer, const char *format, ...)
{
    va_list arguments;
    int written;

    if (buffer == NULL || format == NULL || !buffer->valid ||
        buffer->size >= sizeof(buffer->bytes)) {
        if (buffer != NULL) {
            buffer->valid = false;
        }
        return;
    }
    va_start(arguments, format);
    written = vsnprintf(
        buffer->bytes + buffer->size,
        sizeof(buffer->bytes) - buffer->size,
        format,
        arguments
    );
    va_end(arguments);
    if (written < 0 ||
        (size_t)written >= sizeof(buffer->bytes) - buffer->size) {
        buffer->valid = false;
        return;
    }
    buffer->size += (size_t)written;
}

static void json_append_hex(
    JsonBuffer *buffer,
    const uint8_t *bytes,
    size_t size
)
{
    char encoded[MEISO_K230D_DRM_SUPERVISOR_SHA256_BYTES * 2U + 1U];

    if (size != MEISO_K230D_DRM_SUPERVISOR_SHA256_BYTES) {
        buffer->valid = false;
        return;
    }
    encode_hex(bytes, size, encoded);
    json_append(buffer, "\"%s\"", encoded);
}

static bool read_regular_fd_digest(
    int fd,
    uint64_t maximum_size,
    uint64_t *out_size,
    uint8_t out_digest[32]
)
{
    struct stat descriptor_stat;
    struct stat final_stat;
    uint8_t *bytes;
    uint8_t extra_byte;
    uint64_t size;
    size_t offset = 0U;
    ssize_t extra_amount;

    if (fd < 0 || out_size == NULL || out_digest == NULL ||
        fstat(fd, &descriptor_stat) != 0 ||
        !S_ISREG(descriptor_stat.st_mode) || descriptor_stat.st_size < 0) {
        return false;
    }
    size = (uint64_t)descriptor_stat.st_size;
    if (size > maximum_size || size > (uint64_t)SIZE_MAX) {
        return false;
    }
    bytes = (uint8_t *)malloc(size == 0U ? 1U : (size_t)size);
    if (bytes == NULL) {
        return false;
    }
    while (offset < (size_t)size) {
        ssize_t amount = pread(
            fd, bytes + offset, (size_t)size - offset, (off_t)offset
        );
        if (amount > 0) {
            offset += (size_t)amount;
            continue;
        }
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        free(bytes);
        return false;
    }
    do {
        extra_amount = pread(fd, &extra_byte, 1U, (off_t)size);
    } while (extra_amount < 0 && errno == EINTR);
    if (extra_amount < 0) {
        free(bytes);
        return false;
    }
    if (extra_amount > 0) {
        errno = EFBIG;
        free(bytes);
        return false;
    }
    if (fstat(fd, &final_stat) != 0) {
        free(bytes);
        return false;
    }
    if (!S_ISREG(final_stat.st_mode) ||
        final_stat.st_dev != descriptor_stat.st_dev ||
        final_stat.st_ino != descriptor_stat.st_ino ||
        final_stat.st_size != descriptor_stat.st_size) {
        errno = ESTALE;
        free(bytes);
        return false;
    }
    meiso_k230d_drm_private_sha256(bytes, (size_t)size, out_digest);
    free(bytes);
    *out_size = size;
    return true;
}

static bool read_path_digest(
    const char *path,
    uint64_t maximum_size,
    uint64_t *out_size,
    uint8_t out_digest[32]
)
{
    int fd;
    bool valid;
    int saved_error;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    valid = read_regular_fd_digest(fd, maximum_size, out_size, out_digest);
    saved_error = errno;
    if (close(fd) != 0) {
        valid = false;
        saved_error = errno;
    }
    errno = saved_error;
    return valid;
}

static bool full_write(int fd, const char *bytes, size_t size)
{
    size_t offset = 0U;

    while (offset < size) {
        ssize_t amount = write(fd, bytes + offset, size - offset);
        if (amount > 0) {
            offset += (size_t)amount;
            continue;
        }
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount == 0) {
            errno = EIO;
        }
        return false;
    }
    return true;
}

static uint32_t normalized_errno(int error)
{
    if (error <= 0 || (uint64_t)error >= UINT32_MAX) {
        return (uint32_t)EIO;
    }
    return (uint32_t)error;
}

static void durable_set(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *output,
    MeisoK230dDrmSupervisorDurableDisposition disposition,
    MeisoK230dDrmSupervisorDurableProgress progress,
    uint32_t error_number
)
{
    memset(output, 0, sizeof(*output));
    output->disposition = disposition;
    output->progress = progress;
    output->error_number = error_number;
}

static void durable_cleanup_failure(
    int directory_fd,
    const char *private_name,
    int original_error,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *output
)
{
    if (unlinkat(directory_fd, private_name, 0) == 0 &&
        fsync(directory_fd) == 0) {
        durable_set(
            output,
            MEISO_K230D_DRM_SUPERVISOR_DURABLE_FAILED_CLEAN,
            MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_NONE,
            normalized_errno(original_error)
        );
    } else {
        durable_set(
            output,
            MEISO_K230D_DRM_SUPERVISOR_DURABLE_STATE_UNCERTAIN,
            MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_UNKNOWN,
            normalized_errno(errno)
        );
    }
}

static int durable_publish(
    int directory_fd,
    const char *private_name,
    const char *final_name,
    const JsonBuffer *body,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *output
)
{
    int fd;
    int operation_error;

    if (directory_fd < 0 || private_name == NULL || final_name == NULL ||
        body == NULL || output == NULL || !body->valid || body->size == 0U) {
        return EINVAL;
    }
    fd = openat(
        directory_fd, private_name,
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR
    );
    if (fd < 0) {
        operation_error = positive_error();
        durable_set(
            output,
            operation_error == EEXIST
                ? MEISO_K230D_DRM_SUPERVISOR_DURABLE_STATE_UNCERTAIN
                : MEISO_K230D_DRM_SUPERVISOR_DURABLE_FAILED_CLEAN,
            operation_error == EEXIST
                ? MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_UNKNOWN
                : MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_NONE,
            normalized_errno(operation_error)
        );
        return 0;
    }
    if (!full_write(fd, body->bytes, body->size)) {
        operation_error = positive_error();
        (void)close(fd);
        durable_cleanup_failure(
            directory_fd, private_name, operation_error, output
        );
        return 0;
    }
    if (fsync(fd) != 0) {
        operation_error = positive_error();
        (void)close(fd);
        durable_cleanup_failure(
            directory_fd, private_name, operation_error, output
        );
        return 0;
    }
    if (close(fd) != 0) {
        durable_set(
            output,
            MEISO_K230D_DRM_SUPERVISOR_DURABLE_STATE_UNCERTAIN,
            MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_BODY_SYNCED,
            normalized_errno(positive_error())
        );
        return 0;
    }
    if (linkat(directory_fd, private_name, directory_fd, final_name, 0) != 0) {
        operation_error = positive_error();
        if (operation_error == EEXIST) {
            if (unlinkat(directory_fd, private_name, 0) == 0 &&
                fsync(directory_fd) == 0) {
                durable_set(
                    output,
                    MEISO_K230D_DRM_SUPERVISOR_DURABLE_ALREADY_EXISTS,
                    MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_NONE,
                    (uint32_t)EEXIST
                );
            } else {
                durable_set(
                    output,
                    MEISO_K230D_DRM_SUPERVISOR_DURABLE_STATE_UNCERTAIN,
                    MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_UNKNOWN,
                    normalized_errno(errno)
                );
            }
        } else {
            durable_cleanup_failure(
                directory_fd, private_name, operation_error, output
            );
        }
        return 0;
    }
    if (unlinkat(directory_fd, private_name, 0) != 0) {
        durable_set(
            output,
            MEISO_K230D_DRM_SUPERVISOR_DURABLE_STATE_UNCERTAIN,
            MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_UNIQUE_NAME_PUBLISHED,
            normalized_errno(positive_error())
        );
        return 0;
    }
    if (fsync(directory_fd) != 0) {
        durable_set(
            output,
            MEISO_K230D_DRM_SUPERVISOR_DURABLE_STATE_UNCERTAIN,
            MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_UNIQUE_NAME_PUBLISHED,
            normalized_errno(positive_error())
        );
        return 0;
    }
    durable_set(
        output,
        MEISO_K230D_DRM_SUPERVISOR_DURABLE_CREATED,
        MEISO_K230D_DRM_SUPERVISOR_DURABLE_PROGRESS_PARENT_DIRECTORY_SYNCED,
        0U
    );
    return 0;
}

static void json_append_binding(
    JsonBuffer *buffer,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding
)
{
    json_append(buffer, "\"run_nonce\":");
    json_append_hex(buffer, binding->run_nonce, sizeof(binding->run_nonce));
    json_append(buffer, ",\"boot_id_sha256\":");
    json_append_hex(
        buffer, binding->expected_boot_id_sha256,
        sizeof(binding->expected_boot_id_sha256)
    );
    json_append(buffer, ",\"artifact_sha256\":");
    json_append_hex(
        buffer, binding->expected_artifact_sha256,
        sizeof(binding->expected_artifact_sha256)
    );
    json_append(buffer, ",\"topology_sha256\":");
    json_append_hex(
        buffer, binding->derive_request.expected_topology_sha256,
        sizeof(binding->derive_request.expected_topology_sha256)
    );
    json_append(
        buffer,
        ",\"connector_id\":%" PRIu32 ",\"crtc_id\":%" PRIu32,
        binding->derive_request.connector_id,
        binding->derive_request.crtc_id
    );
}

static void json_append_durable(
    JsonBuffer *buffer,
    const char *prefix,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *value
)
{
    json_append(
        buffer,
        ",\"%s_disposition\":%u,\"%s_progress\":%u,"
        "\"%s_error\":%" PRIu32,
        prefix, (unsigned int)value->disposition,
        prefix, (unsigned int)value->progress,
        prefix, value->error_number
    );
}

static void json_append_result(
    JsonBuffer *buffer,
    const char *record_state,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *result
)
{
    json_append(
        buffer,
        "{\"type\":\"k230d_drm_single_plane_test_only_supervision\","
        "\"schema\":\"meiso.k230d_drm_single_plane_test_only_supervision.v1\","
        "\"record_state\":\"%s\",",
        record_state
    );
    json_append_binding(buffer, &result->binding);
    json_append(
        buffer,
        ",\"outcome\":%u,\"first_failure\":%u,"
        "\"first_failure_error\":%" PRIu32
        ",\"child_pid\":%" PRIu32
        ",\"child_spawned\":%u,\"child_ready_and_stopped\":%u,"
        "\"child_gate_released\":%u,"
        "\"child_release_state_uncertain\":%u,\"child_reaped\":%u,"
        "\"stop_requested\":%u,\"counters_saturated\":%u",
        (unsigned int)result->outcome,
        (unsigned int)result->first_failure,
        result->first_failure_error,
        result->child_pid,
        (unsigned int)result->child_spawned,
        (unsigned int)result->child_ready_and_stopped,
        (unsigned int)result->child_gate_released,
        (unsigned int)result->child_release_state_uncertain,
        (unsigned int)result->child_reaped,
        (unsigned int)result->stop_requested,
        (unsigned int)result->counters_saturated
    );
    json_append_durable(buffer, "global_claim", &result->global_claim);
    json_append_durable(buffer, "evidence_claim", &result->evidence_claim);
    json_append_durable(buffer, "ready_record", &result->ready_record);
    json_append_durable(
        buffer, "quarantine_record", &result->quarantine_record
    );
    json_append(
        buffer,
        ",\"preflight_call_count\":%" PRIu32
        ",\"preflight_0_boot_matches\":%u,"
        "\"preflight_0_artifact_matches\":%u,"
        "\"preflight_0_node_matches\":%u,"
        "\"preflight_0_holder_scan_complete\":%u,"
        "\"preflight_0_holder_count\":%" PRIu32
        ",\"preflight_0_single_thread\":%u,"
        "\"preflight_1_boot_matches\":%u,"
        "\"preflight_1_artifact_matches\":%u,"
        "\"preflight_1_node_matches\":%u,"
        "\"preflight_1_holder_scan_complete\":%u,"
        "\"preflight_1_holder_count\":%" PRIu32
        ",\"preflight_1_single_thread\":%u",
        result->preflight_call_count,
        (unsigned int)result->preflight[0].boot_identity_matches,
        (unsigned int)result->preflight[0].artifact_identity_matches,
        (unsigned int)result->preflight[0].primary_node_identity_matches,
        (unsigned int)result->preflight[0].holder_scan_complete,
        result->preflight[0].primary_holder_count,
        (unsigned int)result->preflight[0].single_thread_verified,
        (unsigned int)result->preflight[1].boot_identity_matches,
        (unsigned int)result->preflight[1].artifact_identity_matches,
        (unsigned int)result->preflight[1].primary_node_identity_matches,
        (unsigned int)result->preflight[1].holder_scan_complete,
        result->preflight[1].primary_holder_count,
        (unsigned int)result->preflight[1].single_thread_verified
    );
    json_append(
        buffer,
        ",\"gate_kind\":%u,\"gate_ready_byte\":%u,"
        "\"gate_exact_wait\":%u,\"gate_exit_valid\":%u,"
        "\"gate_exit_code\":%u,\"gate_signal\":%u,"
        "\"child_start_ticks_valid\":%u,"
        "\"child_start_ticks\":%" PRIu64
        ",\"release_kind\":%u,\"sigcont_dispatch_known\":%u,"
        "\"sigcont_dispatched\":%u,\"t0_valid\":%u,"
        "\"release_error\":%" PRIu32
        ",\"post_cont_t0_ns\":%" PRIu64,
        (unsigned int)result->gate.kind,
        (unsigned int)result->gate.ready_byte_observed,
        (unsigned int)result->gate.exact_wait_child_matched,
        (unsigned int)result->gate.exit_code_valid,
        (unsigned int)result->gate.child_exit_code,
        (unsigned int)result->gate.child_signal,
        (unsigned int)result->gate.child_start_ticks_valid,
        result->gate.child_start_ticks,
        (unsigned int)result->release.kind,
        (unsigned int)result->release.sigcont_dispatch_known,
        (unsigned int)result->release.sigcont_dispatched,
        (unsigned int)result->release.t0_valid,
        result->release.error_number,
        result->release.post_cont_t0_ns
    );
    json_append(
        buffer,
        ",\"term_deadline_ns\":%" PRIu64
        ",\"kill_deadline_ns\":%" PRIu64
        ",\"quarantine_deadline_ns\":%" PRIu64
        ",\"term_observed_now_ns\":%" PRIu64
        ",\"kill_observed_now_ns\":%" PRIu64
        ",\"quarantine_observed_now_ns\":%" PRIu64
        ",\"term_deadline_confirmed\":%u,"
        "\"kill_deadline_confirmed\":%u,"
        "\"quarantine_deadline_confirmed\":%u,"
        "\"timed_wait_call_count\":%" PRIu32
        ",\"indefinite_wait_call_count\":%" PRIu32
        ",\"interrupted_wait_count\":%" PRIu32
        ",\"stop_request_count\":%" PRIu32
        ",\"terminate_call_count\":%" PRIu32
        ",\"term_error\":%" PRIu32
        ",\"kill_call_count\":%" PRIu32
        ",\"kill_error\":%" PRIu32,
        result->term_deadline_ns,
        result->kill_deadline_ns,
        result->quarantine_deadline_ns,
        result->term_deadline_observed_now_ns,
        result->kill_deadline_observed_now_ns,
        result->quarantine_deadline_observed_now_ns,
        (unsigned int)result->term_deadline_confirmed,
        (unsigned int)result->kill_deadline_confirmed,
        (unsigned int)result->quarantine_deadline_confirmed,
        result->timed_wait_call_count,
        result->indefinite_wait_call_count,
        result->interrupted_wait_count,
        result->stop_request_count,
        result->terminate_call_count,
        result->term_error,
        result->kill_call_count,
        result->kill_error
    );
    json_append(
        buffer,
        ",\"final_reap_kind\":%u,\"final_exit_valid\":%u,"
        "\"final_exit_code\":%u,\"final_signal\":%u,"
        "\"postflight_attempted\":%u,\"postflight_succeeded\":%u,"
        "\"post_boot_matches\":%u,\"post_artifact_matches\":%u,"
        "\"post_node_matches\":%u,\"post_holder_scan_complete\":%u,"
        "\"post_holder_count\":%" PRIu32
        ",\"stdout_bytes\":%" PRIu64
        ",\"stderr_bytes\":%" PRIu64
        ",\"stdout_complete\":%u,\"stderr_complete\":%u,"
        "\"stdout_hash_valid\":%u,\"stderr_hash_valid\":%u,"
        "\"stdout_synced\":%u,\"stderr_synced\":%u,"
        "\"stdout_closed\":%u,\"stderr_closed\":%u,"
        "\"stdout_sha256\":",
        (unsigned int)result->final_reap.kind,
        (unsigned int)result->final_reap.exit_code_valid,
        (unsigned int)result->final_reap.child_exit_code,
        (unsigned int)result->final_reap.child_signal,
        (unsigned int)result->postflight_attempted,
        (unsigned int)result->postflight_succeeded,
        (unsigned int)result->postflight.boot_identity_matches,
        (unsigned int)result->postflight.artifact_identity_matches,
        (unsigned int)result->postflight.primary_node_identity_matches,
        (unsigned int)result->postflight.holder_scan_complete,
        result->postflight.primary_holder_count,
        result->postflight.stdout_bytes,
        result->postflight.stderr_bytes,
        (unsigned int)result->postflight.stdout_complete,
        (unsigned int)result->postflight.stderr_complete,
        (unsigned int)result->postflight.stdout_hash_valid,
        (unsigned int)result->postflight.stderr_hash_valid,
        (unsigned int)result->postflight.stdout_synced,
        (unsigned int)result->postflight.stderr_synced,
        (unsigned int)result->postflight.stdout_closed,
        (unsigned int)result->postflight.stderr_closed
    );
    json_append_hex(
        buffer, result->postflight.stdout_sha256,
        sizeof(result->postflight.stdout_sha256)
    );
    json_append(buffer, ",\"stderr_sha256\":");
    json_append_hex(
        buffer, result->postflight.stderr_sha256,
        sizeof(result->postflight.stderr_sha256)
    );
    json_append(
        buffer,
        ",\"initial_preflight_callback_error\":%" PRIu32
        ",\"global_claim_callback_error\":%" PRIu32
        ",\"evidence_claim_callback_error\":%" PRIu32
        ",\"spawn_callback_error\":%" PRIu32
        ",\"gate_callback_error\":%" PRIu32
        ",\"pre_release_preflight_callback_error\":%" PRIu32
        ",\"ready_record_callback_error\":%" PRIu32
        ",\"release_callback_error\":%" PRIu32
        ",\"first_clock_callback_error\":%" PRIu32
        ",\"first_wait_callback_error\":%" PRIu32
        ",\"quarantine_record_callback_error\":%" PRIu32
        ",\"postflight_callback_error\":%" PRIu32
        ",\"terminal_record_callback_error\":%" PRIu32 "}\n",
        result->initial_preflight_callback_error,
        result->global_claim_callback_error,
        result->evidence_claim_callback_error,
        result->spawn_callback_error,
        result->gate_callback_error,
        result->pre_release_preflight_callback_error,
        result->ready_record_callback_error,
        result->release_callback_error,
        result->first_clock_callback_error,
        result->first_wait_callback_error,
        result->quarantine_record_callback_error,
        result->postflight_callback_error,
        result->terminal_record_callback_error
    );
}

static void build_claim_body(
    JsonBuffer *buffer,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
    const char *scope
)
{
    json_init(buffer);
    json_append(
        buffer,
        "{\"type\":\"k230d_drm_single_plane_test_only_launch_claim\","
        "\"schema\":\"meiso.k230d_drm_single_plane_test_only_launch_claim.v1\","
        "\"scope\":\"%s\",",
        scope
    );
    json_append_binding(buffer, binding);
    json_append(buffer, "}\n");
}

static bool name_for_private_body(
    const LinuxSupervisorContext *context,
    const char *tag,
    char *output,
    size_t capacity
)
{
    int written = snprintf(
        output, capacity, ".%s.%s.private", tag, context->nonce_hex
    );
    return written > 0 && (size_t)written < capacity;
}

static int claim_global_launch(
    void *opaque,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *output
)
{
    LinuxSupervisorContext *context = (LinuxSupervisorContext *)opaque;
    JsonBuffer body;
    char private_name[160];

    if (context == NULL || binding == NULL || output == NULL ||
        memcmp(binding, &context->binding, sizeof(*binding)) != 0 ||
        !name_for_private_body(
            context, "global-launch", private_name, sizeof(private_name)
        )) {
        return EINVAL;
    }
    build_claim_body(&body, binding, "PER_BOOT_GLOBAL");
    return durable_publish(
        context->global_directory_fd, private_name, GLOBAL_RECORD,
        &body, output
    );
}

static int claim_evidence_launch(
    void *opaque,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *output
)
{
    LinuxSupervisorContext *context = (LinuxSupervisorContext *)opaque;
    JsonBuffer body;
    char private_name[160];

    if (context == NULL || binding == NULL || output == NULL ||
        memcmp(binding, &context->binding, sizeof(*binding)) != 0 ||
        !name_for_private_body(
            context, "evidence-launch", private_name, sizeof(private_name)
        )) {
        return EINVAL;
    }
    build_claim_body(&body, binding, "RUN_DIRECTORY_MIRROR");
    return durable_publish(
        context->run_directory_fd, private_name, EVIDENCE_RECORD,
        &body, output
    );
}

static int publish_result_record(
    LinuxSupervisorContext *context,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *result,
    const char *state,
    const char *tag,
    const char *final_name,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *output
)
{
    JsonBuffer body;
    char private_name[160];

    if (context == NULL || result == NULL || output == NULL ||
        memcmp(&result->binding, &context->binding,
            sizeof(result->binding)) != 0 ||
        !name_for_private_body(
            context, tag, private_name, sizeof(private_name)
        )) {
        return EINVAL;
    }
    json_init(&body);
    json_append_result(&body, state, result);
    return durable_publish(
        context->run_directory_fd, private_name, final_name, &body, output
    );
}

static int publish_ready_record(
    void *opaque,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *result,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *output
)
{
    return publish_result_record(
        (LinuxSupervisorContext *)opaque, result, "READY_AND_STOPPED",
        "ready", READY_RECORD, output
    );
}

static int publish_quarantine_record(
    void *opaque,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *result,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *output
)
{
    const char *state;

    state = result != NULL && result->quarantine_deadline_confirmed == 1U
        ? "QUARANTINED_UNREAPED_AT_DEADLINE"
        : "OWNERSHIP_FENCE_PENDING_EXACT_REAP";
    return publish_result_record(
        (LinuxSupervisorContext *)opaque, result, state,
        "quarantine", QUARANTINE_RECORD, output
    );
}

static int publish_terminal_record(
    void *opaque,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *result,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorDurableCreate *output
)
{
    return publish_result_record(
        (LinuxSupervisorContext *)opaque, result, "REAPED_POSTFLIGHT",
        "result", TERMINAL_RECORD, output
    );
}

static bool read_small_path_digest(
    const char *path,
    uint8_t digest[32]
)
{
    uint8_t bytes[4096];
    size_t size = 0U;
    int fd;
    bool valid = true;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    while (size < sizeof(bytes)) {
        ssize_t amount = read(fd, bytes + size, sizeof(bytes) - size);
        if (amount > 0) {
            size += (size_t)amount;
            continue;
        }
        if (amount == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        valid = false;
        break;
    }
    if (valid && size == sizeof(bytes)) {
        uint8_t extra;
        ssize_t amount;
        do {
            amount = read(fd, &extra, 1U);
        } while (amount < 0 && errno == EINTR);
        if (amount != 0) {
            valid = false;
        }
    }
    if (close(fd) != 0) {
        valid = false;
    }
    if (!valid || size == 0U) {
        return false;
    }
    meiso_k230d_drm_private_sha256(bytes, size, digest);
    return true;
}

static bool numeric_name(const char *text)
{
    size_t index;

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
    }
    return true;
}

static bool count_process_threads(uint32_t *out_count)
{
    DIR *directory;
    struct dirent *entry;
    uint32_t count = 0U;
    int saved_error = 0;

    directory = opendir("/proc/self/task");
    if (directory == NULL) {
        return false;
    }
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (numeric_name(entry->d_name)) {
            if (count == UINT32_MAX - 1U) {
                saved_error = EOVERFLOW;
                break;
            }
            ++count;
        }
        errno = 0;
    }
    if (saved_error == 0 && errno != 0) {
        saved_error = errno;
    }
    if (closedir(directory) != 0 && saved_error == 0) {
        saved_error = errno;
    }
    if (saved_error != 0 || count == 0U) {
        errno = saved_error != 0 ? saved_error : EPROTO;
        return false;
    }
    *out_count = count;
    return true;
}

static bool process_vanished(const char *pid_name)
{
    char path[64];
    struct stat process_stat;
    int written = snprintf(path, sizeof(path), "/proc/%s", pid_name);

    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return false;
    }
    if (lstat(path, &process_stat) == 0) {
        return false;
    }
    return errno == ENOENT || errno == ESRCH;
}

static bool count_primary_holders(dev_t device, uint32_t *out_count)
{
    DIR *processes;
    struct dirent *process_entry;
    uint32_t count = 0U;
    int scan_error = 0;

    processes = opendir("/proc");
    if (processes == NULL) {
        return false;
    }
    errno = 0;
    while ((process_entry = readdir(processes)) != NULL) {
        char descriptor_directory_path[80];
        DIR *descriptors;
        struct dirent *descriptor_entry;
        int written;

        if (!numeric_name(process_entry->d_name)) {
            errno = 0;
            continue;
        }
        written = snprintf(
            descriptor_directory_path, sizeof(descriptor_directory_path),
            "/proc/%s/fd", process_entry->d_name
        );
        if (written <= 0 ||
            (size_t)written >= sizeof(descriptor_directory_path)) {
            scan_error = EOVERFLOW;
            break;
        }
        descriptors = opendir(descriptor_directory_path);
        if (descriptors == NULL) {
            int open_error = positive_error();
            if ((open_error == ENOENT || open_error == ESRCH) &&
                process_vanished(process_entry->d_name)) {
                errno = 0;
                continue;
            }
            scan_error = open_error;
            break;
        }
        errno = 0;
        while ((descriptor_entry = readdir(descriptors)) != NULL) {
            char descriptor_path[128];
            struct stat descriptor_stat;

            if (!numeric_name(descriptor_entry->d_name)) {
                errno = 0;
                continue;
            }
            written = snprintf(
                descriptor_path, sizeof(descriptor_path), "%s/%s",
                descriptor_directory_path, descriptor_entry->d_name
            );
            if (written <= 0 || (size_t)written >= sizeof(descriptor_path)) {
                scan_error = EOVERFLOW;
                break;
            }
            if (stat(descriptor_path, &descriptor_stat) != 0) {
                if (errno == ENOENT || errno == ESRCH) {
                    errno = 0;
                    continue;
                }
                scan_error = positive_error();
                break;
            }
            if (S_ISCHR(descriptor_stat.st_mode) &&
                descriptor_stat.st_rdev == device) {
                if (count == UINT32_MAX - 1U) {
                    scan_error = EOVERFLOW;
                    break;
                }
                ++count;
            }
            errno = 0;
        }
        if (scan_error == 0 && errno != 0) {
            scan_error = errno;
        }
        if (closedir(descriptors) != 0 && scan_error == 0) {
            scan_error = errno;
        }
        if (scan_error != 0) {
            break;
        }
        errno = 0;
    }
    if (scan_error == 0 && errno != 0) {
        scan_error = errno;
    }
    if (closedir(processes) != 0 && scan_error == 0) {
        scan_error = errno;
    }
    if (scan_error != 0) {
        errno = scan_error;
        return false;
    }
    *out_count = count;
    return true;
}

static bool inspect_primary_node(
    LinuxSupervisorContext *context,
    bool establish_identity,
    bool *out_matches
)
{
    struct stat node_stat;

    if (lstat(PRIMARY_NODE, &node_stat) != 0) {
        return false;
    }
    if (!S_ISCHR(node_stat.st_mode)) {
        errno = ENODEV;
        return false;
    }
    if (establish_identity && !context->primary_device_known) {
        context->primary_device = node_stat.st_rdev;
        context->primary_device_known = true;
    }
    *out_matches = context->primary_device_known &&
        node_stat.st_rdev == context->primary_device;
    return true;
}

static int inspect_preflight(
    void *opaque,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
    uint8_t preflight_index,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight *output
)
{
    LinuxSupervisorContext *context = (LinuxSupervisorContext *)opaque;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPreflight candidate;
    uint8_t boot_digest[32];
    uint8_t artifact_digest[32];
    uint64_t artifact_size;
    uint32_t thread_count;
    uint32_t holder_count;
    bool node_matches;

    if (context == NULL || binding == NULL || output == NULL ||
        preflight_index > 1U ||
        memcmp(binding, &context->binding, sizeof(*binding)) != 0) {
        return EINVAL;
    }
    if (!read_small_path_digest(
            "/proc/sys/kernel/random/boot_id", boot_digest
        ) || !read_path_digest(
            "/proc/self/exe", MAX_HASHED_FILE_BYTES,
            &artifact_size, artifact_digest
        ) || artifact_size == 0U ||
        !inspect_primary_node(
            context, preflight_index == 0U, &node_matches
        ) || !count_process_threads(&thread_count) ||
        !count_primary_holders(context->primary_device, &holder_count)) {
        return positive_error();
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.boot_identity_matches =
        memcmp(
            boot_digest, binding->expected_boot_id_sha256,
            sizeof(boot_digest)
        ) == 0 ? 1U : 0U;
    candidate.artifact_identity_matches =
        memcmp(
            artifact_digest, binding->expected_artifact_sha256,
            sizeof(artifact_digest)
        ) == 0 ? 1U : 0U;
    candidate.primary_node_identity_matches = node_matches ? 1U : 0U;
    candidate.holder_scan_complete = 1U;
    candidate.single_thread_verified = thread_count == 1U ? 1U : 0U;
    candidate.primary_holder_count = holder_count;
    memcpy(output, &candidate, sizeof(candidate));
    return 0;
}

static int child_read_parent_id(void *opaque, uint32_t *out_parent_id)
{
    pid_t parent;

    if (opaque == NULL || out_parent_id == NULL) {
        return EINVAL;
    }
    parent = getppid();
    if (parent <= 0) {
        return EPROTO;
    }
    *out_parent_id = (uint32_t)parent;
    return 0;
}

static int child_arm_parent_death_kill(void *opaque)
{
    if (opaque == NULL) {
        return EINVAL;
    }
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
        return positive_error();
    }
    return 0;
}

static int child_isolate_file_descriptors(void *opaque)
{
    LinuxChildContext *child = (LinuxChildContext *)opaque;
    LinuxSupervisorContext *context;
    DIR *directory;
    struct dirent *entry;
    int *descriptors = NULL;
    size_t descriptor_count = 0U;
    size_t descriptor_capacity = 0U;
    size_t descriptor_index;
    int directory_fd;
    int null_fd;
    int scan_error = 0;

    if (child == NULL || child->supervisor == NULL) {
        return EINVAL;
    }
    context = child->supervisor;
    null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (null_fd < 0) {
        return positive_error();
    }
    if (dup2(null_fd, STDIN_FILENO) < 0 ||
        dup2(context->stdout_fd, STDOUT_FILENO) < 0 ||
        dup2(context->stderr_fd, STDERR_FILENO) < 0) {
        int error = positive_error();
        (void)close(null_fd);
        return error;
    }
    if (setvbuf(stdout, NULL, _IONBF, 0) != 0 ||
        setvbuf(stderr, NULL, _IONBF, 0) != 0) {
        return EIO;
    }
    if (null_fd > STDERR_FILENO && close(null_fd) != 0) {
        return positive_error();
    }
    if (context->ready_write_fd <= STDERR_FILENO) {
        return EPROTO;
    }
    directory = opendir("/proc/self/fd");
    if (directory == NULL) {
        return positive_error();
    }
    directory_fd = dirfd(directory);
    if (directory_fd < 0) {
        scan_error = positive_error();
    }
    errno = 0;
    while (scan_error == 0 && (entry = readdir(directory)) != NULL) {
        char *end;
        long descriptor;
        int *grown;

        if (!numeric_name(entry->d_name)) {
            errno = 0;
            continue;
        }
        errno = 0;
        descriptor = strtol(entry->d_name, &end, 10);
        if (errno != 0 || end == entry->d_name || *end != '\0' ||
            descriptor < 0 || descriptor > INT_MAX) {
            scan_error = EPROTO;
            break;
        }
        if (descriptor <= STDERR_FILENO ||
            descriptor == context->ready_write_fd ||
            descriptor == directory_fd) {
            errno = 0;
            continue;
        }
        if (descriptor_count == descriptor_capacity) {
            size_t new_capacity = descriptor_capacity == 0U ?
                16U : descriptor_capacity * 2U;
            if (new_capacity < descriptor_capacity ||
                new_capacity > SIZE_MAX / sizeof(*descriptors)) {
                scan_error = EOVERFLOW;
                break;
            }
            grown = (int *)realloc(
                descriptors, new_capacity * sizeof(*descriptors)
            );
            if (grown == NULL) {
                scan_error = ENOMEM;
                break;
            }
            descriptors = grown;
            descriptor_capacity = new_capacity;
        }
        descriptors[descriptor_count++] = (int)descriptor;
        errno = 0;
    }
    if (scan_error == 0 && errno != 0) {
        scan_error = positive_error();
    }
    if (closedir(directory) != 0 && scan_error == 0) {
        scan_error = positive_error();
    }
    if (scan_error != 0) {
        free(descriptors);
        return scan_error;
    }
    for (descriptor_index = 0U; descriptor_index < descriptor_count;
         ++descriptor_index) {
        if (close(descriptors[descriptor_index]) != 0 &&
            errno != EBADF && errno != EINTR) {
            scan_error = positive_error();
            break;
        }
    }
    free(descriptors);
    if (scan_error != 0) {
        return scan_error;
    }
    directory = opendir("/proc/self/fd");
    if (directory == NULL) {
        return positive_error();
    }
    directory_fd = dirfd(directory);
    if (directory_fd < 0) {
        scan_error = positive_error();
    }
    errno = 0;
    while (scan_error == 0 && (entry = readdir(directory)) != NULL) {
        char *end;
        long descriptor;

        if (!numeric_name(entry->d_name)) {
            errno = 0;
            continue;
        }
        errno = 0;
        descriptor = strtol(entry->d_name, &end, 10);
        if (errno != 0 || end == entry->d_name || *end != '\0' ||
            descriptor < 0 || descriptor > INT_MAX) {
            scan_error = EPROTO;
            break;
        }
        if (descriptor > STDERR_FILENO &&
            descriptor != context->ready_write_fd &&
            descriptor != directory_fd) {
            scan_error = EPROTO;
            break;
        }
        errno = 0;
    }
    if (scan_error == 0 && errno != 0) {
        scan_error = positive_error();
    }
    if (closedir(directory) != 0 && scan_error == 0) {
        scan_error = positive_error();
    }
    if (scan_error != 0) {
        return scan_error;
    }
    context->stdout_fd = STDOUT_FILENO;
    context->stderr_fd = STDERR_FILENO;
    return 0;
}

static int child_reset_signals(void *opaque)
{
    struct sigaction action;
    sigset_t empty;
    int signal_number;

    if (opaque == NULL) {
        return EINVAL;
    }
    for (signal_number = 1; signal_number < NSIG; ++signal_number) {
        if (signal_number == SIGKILL || signal_number == SIGSTOP) {
            continue;
        }
        memset(&action, 0, sizeof(action));
        action.sa_handler = signal_number == SIGPIPE ? SIG_IGN : SIG_DFL;
        if (sigemptyset(&action.sa_mask) != 0) {
            return positive_error();
        }
        if (sigaction(signal_number, &action, NULL) != 0 && errno != EINVAL) {
            return positive_error();
        }
    }
    if (sigemptyset(&empty) != 0 ||
        sigprocmask(SIG_SETMASK, &empty, NULL) != 0) {
        return positive_error();
    }
    return 0;
}

static int child_notify_ready(void *opaque)
{
    LinuxChildContext *child = (LinuxChildContext *)opaque;
    LinuxSupervisorContext *context;
    uint8_t ready = MEISO_K230D_DRM_SUPERVISOR_READY_BYTE;
    ssize_t amount;

    if (child == NULL || child->supervisor == NULL) {
        return EINVAL;
    }
    context = child->supervisor;
    do {
        amount = write(context->ready_write_fd, &ready, sizeof(ready));
    } while (amount < 0 && errno == EINTR);
    if (amount != (ssize_t)sizeof(ready)) {
        return amount < 0 ? positive_error() : EIO;
    }
    if (close(context->ready_write_fd) != 0) {
        return positive_error();
    }
    context->ready_write_fd = -1;
    return 0;
}

static int child_stop_self(void *opaque)
{
    if (opaque == NULL) {
        return EINVAL;
    }
    if (raise(SIGSTOP) != 0) {
        return positive_error();
    }
    return 0;
}

static int child_run_derived_v2(
    void *opaque,
    const MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest *request,
    uint8_t *out_exit_code
)
{
    LinuxChildContext *child = (LinuxChildContext *)opaque;
    char topology_hex[65];
    char connector[11];
    char crtc[11];
    char *arguments[10];
    int written_connector;
    int written_crtc;
    int result;

    if (child == NULL || request == NULL || out_exit_code == NULL ||
        memcmp(
            request, &child->supervisor->binding.derive_request,
            sizeof(*request)
        ) != 0) {
        return EPROTO;
    }
    encode_hex(
        request->expected_topology_sha256,
        sizeof(request->expected_topology_sha256), topology_hex
    );
    written_connector = snprintf(
        connector, sizeof(connector), "%" PRIu32, request->connector_id
    );
    written_crtc = snprintf(
        crtc, sizeof(crtc), "%" PRIu32, request->crtc_id
    );
    if (written_connector <= 0 ||
        (size_t)written_connector >= sizeof(connector) ||
        written_crtc <= 0 || (size_t)written_crtc >= sizeof(crtc)) {
        return EPROTO;
    }
    arguments[0] = (char *)"meiso-supervised-direct-derived-v2";
    arguments[1] = (char *)"--derive-unique-plane";
    arguments[2] = (char *)"--device";
    arguments[3] = (char *)PRIMARY_NODE;
    arguments[4] = (char *)"--topology-sha256";
    arguments[5] = topology_hex;
    arguments[6] = (char *)"--connector-id";
    arguments[7] = connector;
    arguments[8] = (char *)"--crtc-id";
    arguments[9] = crtc;
    result =
        meiso_k230d_drm_single_plane_test_only_once_derive_unique_plane_run(
            stdout, stderr, 10, arguments,
            meiso_k230d_drm_libdrm_run_single_plane_test_only_derive_unique_plane
        );
    if (result < 0 || result > 2) {
        return EPROTO;
    }
    *out_exit_code = (uint8_t)result;
    return 0;
}

static const MeisoK230dDrmSinglePlaneTestOnlySupervisorChildOps CHILD_OPS = {
    child_read_parent_id,
    child_arm_parent_death_kill,
    child_isolate_file_descriptors,
    child_reset_signals,
    child_notify_ready,
    child_stop_self,
    child_run_derived_v2
};

static void run_child_process(
    LinuxSupervisorContext *context,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding
)
{
    LinuxChildContext child;
    MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest request;
    MeisoK230dDrmSupervisorChildStatus status;
    uint8_t exit_code = UINT8_MAX;
    int final_exit = CHILD_SETUP_FAILURE_EXIT;

    memset(&child, 0, sizeof(child));
    child.supervisor = context;
    memcpy(&request, &binding->derive_request, sizeof(request));
    status = meiso_k230d_drm_private_run_single_plane_test_only_child(
        (uint32_t)context->supervisor_pid,
        &request,
        &CHILD_OPS,
        &child,
        &exit_code
    );
    if (status == MEISO_K230D_DRM_SUPERVISOR_CHILD_DERIVED_RETURNED &&
        exit_code <= 1U) {
        final_exit = (int)exit_code;
    }
    (void)fflush(stdout);
    (void)fflush(stderr);
    (void)close(STDOUT_FILENO);
    (void)close(STDERR_FILENO);
    _exit(final_exit);
}

static int create_raw_output(
    int directory_fd,
    const char *name
)
{
    return openat(
        directory_fd, name,
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR
    );
}

static int spawn_one_child(
    void *opaque,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
    uint32_t *out_child_pid
)
{
    LinuxSupervisorContext *context = (LinuxSupervisorContext *)opaque;
    int ready_pipe[2] = {-1, -1};
    pid_t child;

    if (context == NULL || binding == NULL || out_child_pid == NULL ||
        memcmp(binding, &context->binding, sizeof(*binding)) != 0) {
        return EINVAL;
    }
    context->stdout_fd = create_raw_output(
        context->run_directory_fd, STDOUT_RECORD
    );
    if (context->stdout_fd < 0) {
        return positive_error();
    }
    context->stderr_fd = create_raw_output(
        context->run_directory_fd, STDERR_RECORD
    );
    if (context->stderr_fd < 0) {
        int error = positive_error();
        (void)close(context->stdout_fd);
        context->stdout_fd = -1;
        return error;
    }
    if (fsync(context->run_directory_fd) != 0 ||
        pipe2(ready_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        int error = positive_error();
        (void)close(context->stdout_fd);
        (void)close(context->stderr_fd);
        context->stdout_fd = -1;
        context->stderr_fd = -1;
        return error;
    }
    context->ready_read_fd = ready_pipe[0];
    context->ready_write_fd = ready_pipe[1];
    child = fork();
    if (child < 0) {
        int error = positive_error();
        (void)close(ready_pipe[0]);
        (void)close(ready_pipe[1]);
        (void)close(context->stdout_fd);
        (void)close(context->stderr_fd);
        context->ready_read_fd = -1;
        context->ready_write_fd = -1;
        context->stdout_fd = -1;
        context->stderr_fd = -1;
        return error;
    }
    if (child == 0) {
        (void)close(context->ready_read_fd);
        context->ready_read_fd = -1;
        run_child_process(context, binding);
    }
    (void)close(context->ready_write_fd);
    context->ready_write_fd = -1;
    context->child_pid = child;
    *out_child_pid = (uint32_t)child;
    return 0;
}

static bool parse_start_ticks(pid_t child_pid, uint64_t *out_ticks)
{
    char path[64];
    char buffer[4096];
    char *cursor;
    char *end;
    size_t size = 0U;
    int fd;
    int written;
    int field;

    written = snprintf(path, sizeof(path), "/proc/%ld/stat", (long)child_pid);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return false;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return false;
    }
    while (size + 1U < sizeof(buffer)) {
        ssize_t amount = read(fd, buffer + size, sizeof(buffer) - size - 1U);
        if (amount > 0) {
            size += (size_t)amount;
            continue;
        }
        if (amount == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        (void)close(fd);
        return false;
    }
    if (close(fd) != 0 || size == 0U || size + 1U >= sizeof(buffer)) {
        return false;
    }
    buffer[size] = '\0';
    cursor = strrchr(buffer, ')');
    if (cursor == NULL || cursor[1] != ' ') {
        return false;
    }
    cursor += 2;
    for (field = 3; field < 22; ++field) {
        cursor = strchr(cursor, ' ');
        if (cursor == NULL) {
            return false;
        }
        ++cursor;
    }
    errno = 0;
    *out_ticks = strtoull(cursor, &end, 10);
    return errno == 0 && end != cursor &&
        (*end == ' ' || *end == '\n' || *end == '\0') &&
        *out_ticks > 0U && *out_ticks != UINT64_MAX;
}

static void gate_set_reaped(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation *output,
    int wait_status,
    bool ready_seen
)
{
    memset(output, 0, sizeof(*output));
    output->ready_byte_observed = ready_seen ? 1U : 0U;
    output->exact_wait_child_matched = 1U;
    if (WIFEXITED(wait_status)) {
        output->kind = MEISO_K230D_DRM_SUPERVISOR_GATE_CHILD_EXITED;
        output->exit_code_valid = 1U;
        output->child_exit_code = (uint16_t)WEXITSTATUS(wait_status);
    } else {
        output->kind = MEISO_K230D_DRM_SUPERVISOR_GATE_CHILD_SIGNALED;
        output->child_signal = (uint8_t)WTERMSIG(wait_status);
    }
}

static int consume_pending_supervisor_signal(
    LinuxSupervisorContext *context,
    const struct timespec *timeout
)
{
    siginfo_t signal_info;

    return sigtimedwait(
        &context->supervised_signals, &signal_info, timeout
    );
}

static int await_child_gate(
    void *opaque,
    uint32_t child_pid,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation *output
)
{
    LinuxSupervisorContext *context = (LinuxSupervisorContext *)opaque;
    struct timespec short_wait = {0, GATE_POLL_NS};

    if (context == NULL || output == NULL || child_pid == 0U ||
        child_pid != (uint32_t)context->child_pid) {
        return EINVAL;
    }
    for (;;) {
        int wait_status = 0;
        pid_t waited;
        uint8_t ready_bytes[2];
        ssize_t amount;
        int signal_number;

        do {
            amount = read(
                context->ready_read_fd, ready_bytes, sizeof(ready_bytes)
            );
        } while (amount < 0 && errno == EINTR);
        if (amount > 0) {
            if (amount != 1 ||
                ready_bytes[0] != MEISO_K230D_DRM_SUPERVISOR_READY_BYTE ||
                context->ready_byte_seen) {
                return EPROTO;
            }
            context->ready_byte_seen = true;
        } else if (amount < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return positive_error();
        }
        waited = waitpid((pid_t)child_pid, &wait_status, WNOHANG | WUNTRACED);
        if (waited == (pid_t)child_pid) {
            if (WIFEXITED(wait_status) || WIFSIGNALED(wait_status)) {
                gate_set_reaped(output, wait_status, context->ready_byte_seen);
                return 0;
            }
            if (WIFSTOPPED(wait_status)) {
                uint64_t start_ticks;
                if (WSTOPSIG(wait_status) != SIGSTOP ||
                    !context->ready_byte_seen ||
                    !parse_start_ticks((pid_t)child_pid, &start_ticks)) {
                    return EPROTO;
                }
                memset(output, 0, sizeof(*output));
                output->kind =
                    MEISO_K230D_DRM_SUPERVISOR_GATE_READY_AND_STOPPED;
                output->ready_byte_observed = 1U;
                output->exact_wait_child_matched = 1U;
                output->child_start_ticks_valid = 1U;
                output->child_start_ticks = start_ticks;
                return 0;
            }
            return EPROTO;
        }
        if (waited < 0) {
            return positive_error();
        }
        signal_number = consume_pending_supervisor_signal(
            context, &short_wait
        );
        if (signal_number == SIGHUP || signal_number == SIGINT ||
            signal_number == SIGTERM) {
            memset(output, 0, sizeof(*output));
            output->kind = MEISO_K230D_DRM_SUPERVISOR_GATE_STOP_REQUESTED;
            output->ready_byte_observed = context->ready_byte_seen ? 1U : 0U;
            return 0;
        }
        if (signal_number < 0 && errno == EINTR) {
            memset(output, 0, sizeof(*output));
            output->kind = MEISO_K230D_DRM_SUPERVISOR_GATE_INTERRUPTED;
            output->ready_byte_observed =
                context->ready_byte_seen ? 1U : 0U;
            return 0;
        }
        if (signal_number < 0 && errno != EAGAIN) {
            return positive_error();
        }
    }
}

static int monotonic_now(void *opaque, uint64_t *out_now_ns)
{
    struct timespec now;
    uint64_t candidate;

    if (opaque == NULL || out_now_ns == NULL) {
        return EINVAL;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return positive_error();
    }
    if (now.tv_sec < 0 || (uint64_t)now.tv_sec >
            (UINT64_MAX - (uint64_t)now.tv_nsec) / UINT64_C(1000000000)) {
        return EOVERFLOW;
    }
    candidate = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec;
    if (candidate == UINT64_MAX) {
        return EOVERFLOW;
    }
    *out_now_ns = candidate;
    return 0;
}

static int release_child_gate(
    void *opaque,
    uint32_t child_pid,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation *output
)
{
    LinuxSupervisorContext *context = (LinuxSupervisorContext *)opaque;
    uint64_t now = UINT64_MAX;
    int clock_error;

    if (context == NULL || output == NULL || child_pid == 0U ||
        child_pid != (uint32_t)context->child_pid) {
        return EINVAL;
    }
    memset(output, 0, sizeof(*output));
    output->post_cont_t0_ns = UINT64_MAX;
    if (kill((pid_t)child_pid, SIGCONT) != 0) {
        output->kind = MEISO_K230D_DRM_SUPERVISOR_RELEASE_NOT_RELEASED;
        output->sigcont_dispatch_known = 1U;
        output->error_number = normalized_errno(positive_error());
        return 0;
    }
    output->sigcont_dispatch_known = 1U;
    output->sigcont_dispatched = 1U;
    clock_error = monotonic_now(context, &now);
    if (clock_error != 0) {
        output->kind = MEISO_K230D_DRM_SUPERVISOR_RELEASED_WITHOUT_T0;
        output->error_number = normalized_errno(clock_error);
        return 0;
    }
    output->kind = MEISO_K230D_DRM_SUPERVISOR_RELEASED_WITH_POST_CONT_T0;
    output->t0_valid = 1U;
    output->post_cont_t0_ns = now;
    return 0;
}

static void wait_set_reaped(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation *output,
    int wait_status
)
{
    memset(output, 0, sizeof(*output));
    if (WIFEXITED(wait_status)) {
        output->kind = MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_EXITED;
        output->exit_code_valid = 1U;
        output->child_exit_code = (uint16_t)WEXITSTATUS(wait_status);
    } else {
        output->kind = MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_SIGNALED;
        output->child_signal = (uint8_t)WTERMSIG(wait_status);
    }
}

static bool relative_timeout(
    LinuxSupervisorContext *context,
    uint64_t deadline_ns,
    struct timespec *out_timeout
)
{
    uint64_t now;
    uint64_t remaining;
    int error;

    error = monotonic_now(context, &now);
    if (error != 0) {
        errno = error;
        return false;
    }
    if (now >= deadline_ns) {
        out_timeout->tv_sec = 0;
        out_timeout->tv_nsec = 0;
        return true;
    }
    remaining = deadline_ns - now;
    out_timeout->tv_sec = (time_t)(remaining / UINT64_C(1000000000));
    out_timeout->tv_nsec = (long)(remaining % UINT64_C(1000000000));
    return true;
}

static int wait_child_until(
    void *opaque,
    uint32_t child_pid,
    uint64_t absolute_deadline_ns,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation *output
)
{
    LinuxSupervisorContext *context = (LinuxSupervisorContext *)opaque;

    if (context == NULL || output == NULL || child_pid == 0U ||
        child_pid != (uint32_t)context->child_pid ||
        absolute_deadline_ns == UINT64_MAX) {
        return EINVAL;
    }
    for (;;) {
        int wait_status = 0;
        pid_t waited = waitpid((pid_t)child_pid, &wait_status, WNOHANG);
        struct timespec timeout;
        int signal_number;

        if (waited == (pid_t)child_pid) {
            if (!WIFEXITED(wait_status) && !WIFSIGNALED(wait_status)) {
                return EPROTO;
            }
            wait_set_reaped(output, wait_status);
            return 0;
        }
        if (waited < 0) {
            return positive_error();
        }
        if (!relative_timeout(context, absolute_deadline_ns, &timeout)) {
            return positive_error();
        }
        if (timeout.tv_sec == 0 && timeout.tv_nsec == 0) {
            memset(output, 0, sizeof(*output));
            output->kind = MEISO_K230D_DRM_SUPERVISOR_WAIT_DEADLINE;
            return 0;
        }
        signal_number = consume_pending_supervisor_signal(context, &timeout);
        if (signal_number == SIGCHLD) {
            continue;
        }
        memset(output, 0, sizeof(*output));
        if (signal_number == SIGHUP || signal_number == SIGINT ||
            signal_number == SIGTERM) {
            output->kind = MEISO_K230D_DRM_SUPERVISOR_WAIT_STOP_REQUESTED;
            return 0;
        }
        if (signal_number < 0 && errno == EAGAIN) {
            output->kind = MEISO_K230D_DRM_SUPERVISOR_WAIT_DEADLINE;
            return 0;
        }
        if (signal_number < 0 && errno == EINTR) {
            output->kind = MEISO_K230D_DRM_SUPERVISOR_WAIT_INTERRUPTED;
            return 0;
        }
        if (signal_number < 0) {
            return positive_error();
        }
    }
}

static int wait_child_to_reap(
    void *opaque,
    uint32_t child_pid,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation *output
)
{
    LinuxSupervisorContext *context = (LinuxSupervisorContext *)opaque;
    int wait_status = 0;
    pid_t waited;

    if (context == NULL || output == NULL || child_pid == 0U ||
        child_pid != (uint32_t)context->child_pid) {
        return EINVAL;
    }
    do {
        waited = waitpid((pid_t)child_pid, &wait_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        return positive_error();
    }
    if (waited != (pid_t)child_pid ||
        (!WIFEXITED(wait_status) && !WIFSIGNALED(wait_status))) {
        return EPROTO;
    }
    wait_set_reaped(output, wait_status);
    return 0;
}

static int terminate_child(void *opaque, uint32_t child_pid)
{
    LinuxSupervisorContext *context = (LinuxSupervisorContext *)opaque;

    if (context == NULL || child_pid == 0U ||
        child_pid != (uint32_t)context->child_pid) {
        return EINVAL;
    }
    if (kill((pid_t)child_pid, SIGTERM) != 0) {
        return positive_error();
    }
    return 0;
}

static int kill_child(void *opaque, uint32_t child_pid)
{
    LinuxSupervisorContext *context = (LinuxSupervisorContext *)opaque;

    if (context == NULL || child_pid == 0U ||
        child_pid != (uint32_t)context->child_pid) {
        return EINVAL;
    }
    if (kill((pid_t)child_pid, SIGKILL) != 0) {
        return positive_error();
    }
    return 0;
}

static bool inspect_stream(
    int fd,
    uint64_t *out_bytes,
    uint8_t out_digest[32],
    uint8_t *out_complete,
    uint8_t *out_hash_valid,
    uint8_t *out_synced,
    uint8_t *out_closed
)
{
    uint64_t size = 0U;
    bool digest_valid = false;
    bool sync_valid = false;
    bool close_valid;

    if (fd < 0) {
        *out_bytes = 0U;
        *out_complete = 0U;
        *out_hash_valid = 0U;
        *out_synced = 0U;
        *out_closed = 0U;
        memset(out_digest, 0, 32U);
        return false;
    }
    sync_valid = fsync(fd) == 0;
    digest_valid = read_regular_fd_digest(
        fd, MAX_RAW_OUTPUT_BYTES, &size, out_digest
    );
    close_valid = close(fd) == 0;
    if (!digest_valid) {
        memset(out_digest, 0, 32U);
        size = 0U;
    }
    *out_bytes = size;
    *out_complete = digest_valid ? 1U : 0U;
    *out_hash_valid = digest_valid ? 1U : 0U;
    *out_synced = sync_valid && digest_valid ? 1U : 0U;
    *out_closed = close_valid ? 1U : 0U;
    return digest_valid && sync_valid && close_valid;
}

static int inspect_postflight(
    void *opaque,
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding,
    uint32_t child_pid,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPostflight *output
)
{
    LinuxSupervisorContext *context = (LinuxSupervisorContext *)opaque;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorPostflight candidate;
    uint8_t boot_digest[32];
    uint8_t artifact_digest[32];
    uint64_t artifact_size = 0U;
    uint32_t holder_count = 0U;
    bool node_matches = false;
    bool boot_valid;
    bool artifact_valid;
    bool node_valid;
    bool holders_valid;

    if (context == NULL || binding == NULL || output == NULL ||
        child_pid == 0U || child_pid != (uint32_t)context->child_pid ||
        memcmp(binding, &context->binding, sizeof(*binding)) != 0) {
        return EINVAL;
    }
    memset(&candidate, 0, sizeof(candidate));
    boot_valid = read_small_path_digest(
        "/proc/sys/kernel/random/boot_id", boot_digest
    );
    artifact_valid = read_path_digest(
        "/proc/self/exe", MAX_HASHED_FILE_BYTES,
        &artifact_size, artifact_digest
    ) && artifact_size > 0U;
    node_valid = inspect_primary_node(context, false, &node_matches);
    holders_valid = node_valid && count_primary_holders(
        context->primary_device, &holder_count
    );
    candidate.boot_identity_matches = boot_valid &&
        memcmp(
            boot_digest, binding->expected_boot_id_sha256,
            sizeof(boot_digest)
        ) == 0 ? 1U : 0U;
    candidate.artifact_identity_matches = artifact_valid &&
        memcmp(
            artifact_digest, binding->expected_artifact_sha256,
            sizeof(artifact_digest)
        ) == 0 ? 1U : 0U;
    candidate.primary_node_identity_matches =
        node_valid && node_matches ? 1U : 0U;
    candidate.holder_scan_complete = holders_valid ? 1U : 0U;
    candidate.primary_holder_count = holders_valid ? holder_count : 0U;
    (void)inspect_stream(
        context->stdout_fd,
        &candidate.stdout_bytes,
        candidate.stdout_sha256,
        &candidate.stdout_complete,
        &candidate.stdout_hash_valid,
        &candidate.stdout_synced,
        &candidate.stdout_closed
    );
    context->stdout_fd = -1;
    (void)inspect_stream(
        context->stderr_fd,
        &candidate.stderr_bytes,
        candidate.stderr_sha256,
        &candidate.stderr_complete,
        &candidate.stderr_hash_valid,
        &candidate.stderr_synced,
        &candidate.stderr_closed
    );
    context->stderr_fd = -1;
    memcpy(output, &candidate, sizeof(candidate));
    return 0;
}

static const MeisoK230dDrmSinglePlaneTestOnlySupervisorOps SUPERVISOR_OPS = {
    inspect_preflight,
    claim_global_launch,
    claim_evidence_launch,
    spawn_one_child,
    await_child_gate,
    publish_ready_record,
    release_child_gate,
    wait_child_until,
    monotonic_now,
    terminate_child,
    kill_child,
    publish_quarantine_record,
    wait_child_to_reap,
    inspect_postflight,
    publish_terminal_record
};

static bool directory_empty(int directory_fd)
{
    int duplicate_fd;
    DIR *directory;
    struct dirent *entry;
    bool empty = true;
    int saved_error = 0;

    duplicate_fd = fcntl(directory_fd, F_DUPFD_CLOEXEC, 3);
    if (duplicate_fd < 0) {
        return false;
    }
    directory = fdopendir(duplicate_fd);
    if (directory == NULL) {
        saved_error = errno;
        (void)close(duplicate_fd);
        errno = saved_error;
        return false;
    }
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
            empty = false;
            errno = 0;
            break;
        }
        errno = 0;
    }
    if (empty && errno != 0) {
        saved_error = errno;
        empty = false;
    }
    if (closedir(directory) != 0) {
        saved_error = errno;
        empty = false;
    }
    if (!empty) {
        errno = saved_error != 0 ? saved_error : ENOTEMPTY;
    }
    return empty;
}

static int open_run_directory(const char *path)
{
    struct stat path_stat;
    struct stat descriptor_stat;
    size_t path_size;
    int fd;

    if (path == NULL || path[0] != '/' ||
        !meiso_k230d_drm_private_path_size(path, &path_size) ||
        lstat(path, &path_stat) != 0 ||
        !S_ISDIR(path_stat.st_mode) || S_ISLNK(path_stat.st_mode) ||
        path_stat.st_uid != geteuid() ||
        (path_stat.st_mode & (mode_t)07777) != (mode_t)0700) {
        return -1;
    }
    (void)path_size;
    fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &descriptor_stat) != 0) {
        int error = positive_error();
        (void)close(fd);
        errno = error;
        return -1;
    }
    if (descriptor_stat.st_dev != path_stat.st_dev ||
        descriptor_stat.st_ino != path_stat.st_ino ||
        !S_ISDIR(descriptor_stat.st_mode) ||
        descriptor_stat.st_uid != geteuid() ||
        (descriptor_stat.st_mode & (mode_t)07777) != (mode_t)0700) {
        (void)close(fd);
        errno = EINVAL;
        return -1;
    }
    if (!directory_empty(fd)) {
        int error = errno != 0 ? errno : ENOTEMPTY;
        (void)close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

static int open_global_directory(void)
{
    struct stat path_stat;
    struct stat descriptor_stat;
    int fd;

    if (lstat(GLOBAL_DIRECTORY, &path_stat) != 0 ||
        !S_ISDIR(path_stat.st_mode) || S_ISLNK(path_stat.st_mode) ||
        path_stat.st_uid != 0U ||
        (path_stat.st_mode & (mode_t)0022) != 0) {
        return -1;
    }
    fd = open(
        GLOBAL_DIRECTORY, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
    );
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &descriptor_stat) != 0) {
        int error = positive_error();
        (void)close(fd);
        errno = error;
        return -1;
    }
    if (descriptor_stat.st_dev != path_stat.st_dev ||
        descriptor_stat.st_ino != path_stat.st_ino ||
        !S_ISDIR(descriptor_stat.st_mode) || descriptor_stat.st_uid != 0U ||
        (descriptor_stat.st_mode & (mode_t)0022) != 0) {
        (void)close(fd);
        errno = EINVAL;
        return -1;
    }
    return fd;
}

static bool isolate_supervisor_process(LinuxSupervisorContext *context)
{
    static const int SIGNALS[] = {SIGHUP, SIGINT, SIGTERM, SIGCHLD};
    struct sigaction action;
    size_t signal_index;
    int null_fd;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    if (sigemptyset(&action.sa_mask) != 0) {
        return false;
    }
    for (signal_index = 0U;
         signal_index < sizeof(SIGNALS) / sizeof(SIGNALS[0]);
         ++signal_index) {
        if (sigaction(SIGNALS[signal_index], &action, NULL) != 0) {
            return false;
        }
    }
    if (sigemptyset(&context->supervised_signals) != 0 ||
        sigaddset(&context->supervised_signals, SIGHUP) != 0 ||
        sigaddset(&context->supervised_signals, SIGINT) != 0 ||
        sigaddset(&context->supervised_signals, SIGTERM) != 0 ||
        sigaddset(&context->supervised_signals, SIGCHLD) != 0 ||
        sigprocmask(SIG_BLOCK, &context->supervised_signals, NULL) != 0) {
        return false;
    }
    null_fd = open("/dev/null", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (null_fd < 0) {
        return false;
    }
    if (dup2(null_fd, STDIN_FILENO) < 0 ||
        dup2(null_fd, STDOUT_FILENO) < 0 ||
        dup2(null_fd, STDERR_FILENO) < 0) {
        int error = positive_error();
        (void)close(null_fd);
        errno = error;
        return false;
    }
    if (null_fd > STDERR_FILENO && close(null_fd) != 0) {
        return false;
    }
    if (getsid(0) != getpid() && setsid() < 0) {
        return false;
    }
    return true;
}

static bool parse_arguments(
    int argument_count,
    char **arguments,
    const char **out_run_directory,
    MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *out_binding
)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding candidate;

    if (argument_count != 15 || arguments == NULL ||
        out_run_directory == NULL || out_binding == NULL ||
        arguments[0] == NULL || arguments[1] == NULL ||
        arguments[2] == NULL || arguments[3] == NULL ||
        arguments[4] == NULL || arguments[5] == NULL ||
        arguments[6] == NULL || arguments[7] == NULL ||
        arguments[8] == NULL || arguments[9] == NULL ||
        arguments[10] == NULL || arguments[11] == NULL ||
        arguments[12] == NULL || arguments[13] == NULL ||
        arguments[14] == NULL ||
        strcmp(arguments[1], "--run-dir") != 0 ||
        strcmp(arguments[3], "--nonce") != 0 ||
        strcmp(arguments[5], "--boot-id-sha256") != 0 ||
        strcmp(arguments[7], "--artifact-sha256") != 0 ||
        strcmp(arguments[9], "--topology-sha256") != 0 ||
        strcmp(arguments[11], "--connector-id") != 0 ||
        strcmp(arguments[13], "--crtc-id") != 0 ||
        arguments[2][0] != '/') {
        return false;
    }
    memset(&candidate, 0, sizeof(candidate));
    if (!decode_lower_hex_32(arguments[4], candidate.run_nonce) ||
        !decode_lower_hex_32(
            arguments[6], candidate.expected_boot_id_sha256
        ) || !decode_lower_hex_32(
            arguments[8], candidate.expected_artifact_sha256
        ) || !decode_lower_hex_32(
            arguments[10],
            candidate.derive_request.expected_topology_sha256
        ) || !parse_canonical_u32(
            arguments[12], &candidate.derive_request.connector_id
        ) || !parse_canonical_u32(
            arguments[14], &candidate.derive_request.crtc_id
        )) {
        return false;
    }
    *out_run_directory = arguments[2];
    memcpy(out_binding, &candidate, sizeof(candidate));
    return true;
}

static void close_context(LinuxSupervisorContext *context)
{
    int *descriptors[] = {
        &context->ready_read_fd,
        &context->ready_write_fd,
        &context->stdout_fd,
        &context->stderr_fd,
        &context->run_directory_fd,
        &context->global_directory_fd
    };
    size_t index;

    for (index = 0U; index < sizeof(descriptors) / sizeof(descriptors[0]);
         ++index) {
        if (*descriptors[index] >= 0) {
            (void)close(*descriptors[index]);
            *descriptors[index] = -1;
        }
    }
}

static bool supervision_succeeded(
    const MeisoK230dDrmSinglePlaneTestOnlySupervisorResult *result
)
{
    return result->first_failure ==
            MEISO_K230D_DRM_SUPERVISOR_FAILURE_NONE &&
        result->counters_saturated == 0U &&
        result->outcome ==
            MEISO_K230D_DRM_SUPERVISOR_OUTCOME_REAPED_WITHOUT_SIGNAL_CALL &&
        result->child_spawned == 1U && result->child_reaped == 1U &&
        result->terminate_call_count == 0U && result->kill_call_count == 0U &&
        result->quarantine_record_attempted == 0U &&
        result->final_reap.kind ==
            MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_EXITED &&
        result->final_reap.exit_code_valid == 1U &&
        result->final_reap.child_exit_code == 0U &&
        result->postflight_succeeded == 1U &&
        result->terminal_record.disposition ==
            MEISO_K230D_DRM_SUPERVISOR_DURABLE_CREATED;
}

int main(int argument_count, char **arguments)
{
    MeisoK230dDrmSinglePlaneTestOnlySupervisorConfig config;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorResult result;
    MeisoK230dDrmSupervisorInvokeStatus status;
    LinuxSupervisorContext context;
    const char *run_directory = NULL;

    memset(&config, 0, sizeof(config));
    memset(&result, 0, sizeof(result));
    memset(&context, 0, sizeof(context));
    context.run_directory_fd = -1;
    context.global_directory_fd = -1;
    context.stdout_fd = -1;
    context.stderr_fd = -1;
    context.ready_read_fd = -1;
    context.ready_write_fd = -1;
    if (!parse_arguments(
            argument_count, arguments, &run_directory, &config.binding
        )) {
        (void)fputs("E_ARGUMENT\n", stderr);
        (void)fflush(stderr);
        return 2;
    }
    context.run_directory_fd = open_run_directory(run_directory);
    if (context.run_directory_fd < 0) {
        (void)fputs("E_ARGUMENT\n", stderr);
        (void)fflush(stderr);
        return 2;
    }
    memcpy(&context.binding, &config.binding, sizeof(context.binding));
    encode_hex(
        config.binding.run_nonce, sizeof(config.binding.run_nonce),
        context.nonce_hex
    );
    context.supervisor_pid = getpid();
    if (context.supervisor_pid <= 0 ||
        !isolate_supervisor_process(&context)) {
        close_context(&context);
        return 1;
    }
    context.global_directory_fd = open_global_directory();
    if (context.global_directory_fd < 0) {
        close_context(&context);
        return 1;
    }
    config.term_after_ns = MEISO_K230D_DRM_SUPERVISOR_TERM_AFTER_NS;
    config.kill_after_ns = MEISO_K230D_DRM_SUPERVISOR_KILL_AFTER_NS;
    config.quarantine_after_ns =
        MEISO_K230D_DRM_SUPERVISOR_QUARANTINE_AFTER_NS;
    status = meiso_k230d_drm_private_supervise_single_plane_test_only_child(
        &config, &SUPERVISOR_OPS, &context, &result
    );
    close_context(&context);
    if (status != MEISO_K230D_DRM_SUPERVISOR_RESULT_AVAILABLE) {
        return 1;
    }
    return supervision_succeeded(&result) ? 0 : 1;
}
