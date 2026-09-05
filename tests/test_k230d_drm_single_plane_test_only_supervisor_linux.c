#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "meiso_drm_libdrm_single_plane_test_only_private.h"
#include "meiso_drm_single_plane_test_only_supervisor_private.h"

static pid_t meiso_test_supervisor_fork(void);

static int meiso_test_derived_once(
    FILE *out_stream,
    FILE *error_stream,
    int argument_count,
    char **arguments,
    MeisoK230dDrmNativeSinglePlaneTestOnlyDeriveUniquePlaneExecuteFn execute
);

static MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus
meiso_test_native_derived_provider(
    const char *device_path,
    const MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest *request,
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult *out_result
);

#define main meiso_test_included_supervisor_main
#define fork meiso_test_supervisor_fork
#define meiso_k230d_drm_single_plane_test_only_once_derive_unique_plane_run \
    meiso_test_derived_once
#define meiso_k230d_drm_libdrm_run_single_plane_test_only_derive_unique_plane \
    meiso_test_native_derived_provider
#include "../targets/k230d-linux/drm_single_plane_test_only_supervisor_linux.c"
#undef meiso_k230d_drm_libdrm_run_single_plane_test_only_derive_unique_plane
#undef meiso_k230d_drm_single_plane_test_only_once_derive_unique_plane_run
#undef fork
#undef main

enum {
    TEST_CONNECTOR_ID = 37,
    TEST_CRTC_ID = 46,
    TEST_WAIT_SLICE_NS = 10000000,
    TEST_PROVIDER_CALLED_EXIT = 97
};

#define TEST_TIMEOUT_NS UINT64_C(1000000000)

static const char CHILD_SUCCESS[] = "HARmless-derived-v2-child\n";
static const char TOPOLOGY_HEX[] =
    "abababababababababababababababab"
    "abababababababababababababababab";

static int forced_fork_error;
static unsigned int supervisor_fork_call_count;
static volatile sig_atomic_t monotonic_alarm_seen;

typedef struct {
    uint32_t child_pid;
    uint64_t child_start_ticks;
    uint8_t stage;
    uint8_t gate_kind;
    uint8_t reserved[6];
} ParentDeathPacket;

typedef struct {
    volatile uint32_t child_pid;
} SharedChildPidSlot;

enum {
    PARENT_DEATH_PACKET_SPAWNED = 1,
    PARENT_DEATH_PACKET_GATED = 2
};

static void fail_at(const char *test_name, int line)
{
    (void)fprintf(stderr, "%s failed at line %d\n", test_name, line);
}

#define TEST_CHECK(test_name, condition) \
    do { \
        if (!(condition)) { \
            fail_at((test_name), __LINE__); \
            ok = false; \
            goto cleanup; \
        } \
    } while (false)

static uint64_t test_monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
        now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) {
        return UINT64_MAX;
    }
    if ((uint64_t)now.tv_sec >
        (UINT64_MAX - (uint64_t)now.tv_nsec) / UINT64_C(1000000000)) {
        return UINT64_MAX;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
        (uint64_t)now.tv_nsec;
}

static void sleep_slice(void)
{
    struct timespec delay = {0, TEST_WAIT_SLICE_NS};

    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static bool bounded_reap(pid_t child, uint64_t timeout_ns, int *out_status)
{
    uint64_t start = test_monotonic_ns();

    if (child <= 0 || start == UINT64_MAX || out_status == NULL ||
        timeout_ns > UINT64_MAX - start) {
        return false;
    }
    for (;;) {
        int status = 0;
        pid_t waited = waitpid(child, &status, WNOHANG);
        uint64_t now;

        if (waited == child) {
            *out_status = status;
            return true;
        }
        if (waited < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        now = test_monotonic_ns();
        if (now == UINT64_MAX || now - start >= timeout_ns) {
            return false;
        }
        sleep_slice();
    }
}

static bool converge_owned_child(pid_t child)
{
    int status = 0;
    pid_t waited;

    if (child <= 0) {
        return true;
    }
    do {
        waited = waitpid(child, &status, WNOHANG);
    } while (waited < 0 && errno == EINTR);
    if (waited == child || (waited < 0 && errno == ECHILD)) {
        return true;
    }
    if (waited < 0) {
        return false;
    }
    if (kill(child, SIGKILL) != 0 && errno != ESRCH) {
        return false;
    }
    if (!bounded_reap(child, TEST_TIMEOUT_NS, &status)) {
        return false;
    }
    errno = 0;
    return waitpid(child, &status, WNOHANG) == (pid_t)-1 && errno == ECHILD;
}

static bool map_shared_child_pid_slot(SharedChildPidSlot **out_slot)
{
    void *mapping;

    if (out_slot == NULL) {
        return false;
    }
    mapping = mmap(
        NULL, sizeof(SharedChildPidSlot), PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0
    );
    if (mapping == MAP_FAILED) {
        return false;
    }
    *out_slot = (SharedChildPidSlot *)mapping;
    (*out_slot)->child_pid = 0U;
    return true;
}

static bool converge_shared_child(const SharedChildPidSlot *slot)
{
    uint32_t child_pid;

    if (slot == NULL) {
        return true;
    }
    child_pid = slot->child_pid;
    if (child_pid == 0U) {
        return true;
    }
    if (child_pid > (uint32_t)INT32_MAX) {
        return false;
    }
    return converge_owned_child((pid_t)child_pid);
}

static bool global_child_empty_fence(void)
{
    uint64_t start = test_monotonic_ns();
    bool unexpected_child_reaped = false;

    if (start == UINT64_MAX || TEST_TIMEOUT_NS > UINT64_MAX - start) {
        return false;
    }
    for (;;) {
        int status = 0;
        pid_t waited;
        uint64_t now;

        errno = 0;
        waited = waitpid((pid_t)-1, &status, WNOHANG | WUNTRACED);
        if (waited > 0) {
            unexpected_child_reaped = true;
            if (!WIFEXITED(status) && !WIFSIGNALED(status)) {
                if (kill(waited, SIGKILL) != 0 && errno != ESRCH) {
                    return false;
                }
                if (!bounded_reap(waited, TEST_TIMEOUT_NS, &status)) {
                    return false;
                }
            }
            continue;
        }
        if (waited < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno == ECHILD && !unexpected_child_reaped;
        }
        now = test_monotonic_ns();
        if (now == UINT64_MAX || now - start >= TEST_TIMEOUT_NS) {
            return false;
        }
        sleep_slice();
    }
}

static bool write_all(int fd, const void *bytes, size_t size)
{
    const uint8_t *cursor = (const uint8_t *)bytes;
    size_t offset = 0U;

    while (offset < size) {
        ssize_t amount = write(fd, cursor + offset, size - offset);
        if (amount > 0) {
            offset += (size_t)amount;
            continue;
        }
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

static bool read_all_with_deadline(
    int fd,
    void *bytes,
    size_t size,
    uint64_t timeout_ns
)
{
    uint8_t *cursor = (uint8_t *)bytes;
    size_t offset = 0U;
    uint64_t start = test_monotonic_ns();

    if (fd < 0 || bytes == NULL || size == 0U || start == UINT64_MAX ||
        timeout_ns > UINT64_MAX - start) {
        return false;
    }
    while (offset < size) {
        ssize_t amount = read(fd, cursor + offset, size - offset);
        uint64_t now;

        if (amount > 0) {
            offset += (size_t)amount;
            continue;
        }
        if (amount == 0) {
            return false;
        }
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            return false;
        }
        now = test_monotonic_ns();
        if (now == UINT64_MAX || now - start >= timeout_ns) {
            return false;
        }
        sleep_slice();
    }
    return true;
}

static void fill_binding(
    MeisoK230dDrmSinglePlaneTestOnlySupervisorBinding *binding
)
{
    size_t index;

    memset(binding, 0, sizeof(*binding));
    for (index = 0U; index < sizeof(binding->run_nonce); ++index) {
        binding->run_nonce[index] = (uint8_t)(index + 1U);
        binding->expected_boot_id_sha256[index] =
            (uint8_t)(UINT8_C(0x40) + index);
        binding->expected_artifact_sha256[index] =
            (uint8_t)(UINT8_C(0x80) + index);
        binding->derive_request.expected_topology_sha256[index] =
            UINT8_C(0xab);
    }
    binding->derive_request.connector_id = TEST_CONNECTOR_ID;
    binding->derive_request.crtc_id = TEST_CRTC_ID;
}

static void initialize_context(
    LinuxSupervisorContext *context,
    int run_directory_fd
)
{
    memset(context, 0, sizeof(*context));
    context->run_directory_fd = run_directory_fd;
    context->global_directory_fd = -1;
    context->stdout_fd = -1;
    context->stderr_fd = -1;
    context->ready_read_fd = -1;
    context->ready_write_fd = -1;
    context->supervisor_pid = getpid();
    context->child_pid = (pid_t)-1;
    fill_binding(&context->binding);
    (void)sigemptyset(&context->supervised_signals);
}

static bool make_test_directory(char path[64], int *out_fd)
{
    char *created;
    int fd;
    int written;

    written = snprintf(
        path, 64U, "%s", "/tmp/meiso-supervisor-linux.XXXXXX"
    );
    if (written <= 0 || written >= 64) {
        return false;
    }
    created = mkdtemp(path);
    if (created == NULL) {
        return false;
    }
    fd = open(created, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        (void)rmdir(created);
        return false;
    }
    *out_fd = fd;
    return true;
}

static void remove_test_directory(const char *path)
{
    char output_path[128];
    int written;

    if (path == NULL || path[0] == '\0') {
        return;
    }
    written = snprintf(output_path, sizeof(output_path), "%s/%s", path,
                       STDOUT_RECORD);
    if (written > 0 && (size_t)written < sizeof(output_path)) {
        (void)unlink(output_path);
    }
    written = snprintf(output_path, sizeof(output_path), "%s/%s", path,
                       STDERR_RECORD);
    if (written > 0 && (size_t)written < sizeof(output_path)) {
        (void)unlink(output_path);
    }
    (void)rmdir(path);
}

static void monotonic_alarm_handler(int signal_number)
{
    if (signal_number == SIGALRM) {
        monotonic_alarm_seen = 1;
    }
}

static bool arm_gate_alarm(
    timer_t *out_timer,
    struct sigaction *out_previous_action
)
{
    struct sigaction action;
    struct sigevent event;
    struct itimerspec timer_value;

    memset(&action, 0, sizeof(action));
    action.sa_handler = monotonic_alarm_handler;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGALRM, &action, out_previous_action) != 0) {
        return false;
    }
    memset(&event, 0, sizeof(event));
    event.sigev_notify = SIGEV_SIGNAL;
    event.sigev_signo = SIGALRM;
    if (timer_create(CLOCK_MONOTONIC, &event, out_timer) != 0) {
        (void)sigaction(SIGALRM, out_previous_action, NULL);
        return false;
    }
    memset(&timer_value, 0, sizeof(timer_value));
    timer_value.it_value.tv_sec =
        (time_t)(TEST_TIMEOUT_NS / UINT64_C(1000000000));
    timer_value.it_value.tv_nsec =
        (long)(TEST_TIMEOUT_NS % UINT64_C(1000000000));
    monotonic_alarm_seen = 0;
    if (timer_settime(*out_timer, 0, &timer_value, NULL) != 0) {
        (void)timer_delete(*out_timer);
        (void)sigaction(SIGALRM, out_previous_action, NULL);
        return false;
    }
    return true;
}

static void disarm_gate_alarm(
    timer_t timer,
    const struct sigaction *previous_action
)
{
    struct itimerspec disabled;

    memset(&disabled, 0, sizeof(disabled));
    (void)timer_settime(timer, 0, &disabled, NULL);
    (void)timer_delete(timer);
    (void)sigaction(SIGALRM, previous_action, NULL);
}

static bool configure_sigchld_wait(
    LinuxSupervisorContext *context,
    sigset_t *out_previous_mask,
    struct sigaction *out_previous_action
)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGCHLD, &action, out_previous_action) != 0) {
        return false;
    }
    if (sigemptyset(&context->supervised_signals) == 0 &&
        sigaddset(&context->supervised_signals, SIGCHLD) == 0 &&
        sigprocmask(
            SIG_BLOCK, &context->supervised_signals, out_previous_mask
        ) == 0) {
        return true;
    }
    (void)sigaction(SIGCHLD, out_previous_action, NULL);
    return false;
}

static void restore_sigchld_wait(
    const sigset_t *previous_mask,
    const struct sigaction *previous_action
)
{
    (void)sigaction(SIGCHLD, previous_action, NULL);
    (void)sigprocmask(SIG_SETMASK, previous_mask, NULL);
}

static bool child_descriptor_set_matches(
    pid_t child_pid,
    const LinuxSupervisorContext *context
)
{
    char directory_path[64];
    char descriptor_path[96];
    DIR *directory = NULL;
    struct dirent *entry;
    struct stat child_stdin;
    struct stat child_stdout;
    struct stat child_stderr;
    struct stat parent_stdout;
    struct stat parent_stderr;
    struct stat null_device;
    unsigned int seen_mask = 0U;
    int written;
    bool valid = false;

    written = snprintf(directory_path, sizeof(directory_path),
                       "/proc/%ld/fd", (long)child_pid);
    if (written <= 0 || (size_t)written >= sizeof(directory_path)) {
        return false;
    }
    directory = opendir(directory_path);
    if (directory == NULL) {
        return false;
    }
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        char *end;
        long descriptor;

        if (!numeric_name(entry->d_name)) {
            errno = 0;
            continue;
        }
        errno = 0;
        descriptor = strtol(entry->d_name, &end, 10);
        if (errno != 0 || end == entry->d_name || *end != '\0' ||
            descriptor < 0 || descriptor > 2) {
            goto cleanup;
        }
        seen_mask |= 1U << (unsigned int)descriptor;
        errno = 0;
    }
    if (errno != 0 || seen_mask != 7U) {
        goto cleanup;
    }
    written = snprintf(descriptor_path, sizeof(descriptor_path),
                       "/proc/%ld/fd/0", (long)child_pid);
    if (written <= 0 || (size_t)written >= sizeof(descriptor_path) ||
        stat(descriptor_path, &child_stdin) != 0 ||
        stat("/dev/null", &null_device) != 0) {
        goto cleanup;
    }
    written = snprintf(descriptor_path, sizeof(descriptor_path),
                       "/proc/%ld/fd/1", (long)child_pid);
    if (written <= 0 || (size_t)written >= sizeof(descriptor_path) ||
        stat(descriptor_path, &child_stdout) != 0) {
        goto cleanup;
    }
    written = snprintf(descriptor_path, sizeof(descriptor_path),
                       "/proc/%ld/fd/2", (long)child_pid);
    if (written <= 0 || (size_t)written >= sizeof(descriptor_path) ||
        stat(descriptor_path, &child_stderr) != 0 ||
        fstat(context->stdout_fd, &parent_stdout) != 0 ||
        fstat(context->stderr_fd, &parent_stderr) != 0) {
        goto cleanup;
    }
    valid = S_ISCHR(child_stdin.st_mode) &&
        child_stdin.st_rdev == null_device.st_rdev &&
        S_ISREG(child_stdout.st_mode) && S_ISREG(child_stderr.st_mode) &&
        child_stdout.st_dev == parent_stdout.st_dev &&
        child_stdout.st_ino == parent_stdout.st_ino &&
        child_stderr.st_dev == parent_stderr.st_dev &&
        child_stderr.st_ino == parent_stderr.st_ino &&
        (child_stdout.st_dev != child_stderr.st_dev ||
         child_stdout.st_ino != child_stderr.st_ino);

cleanup:
    (void)closedir(directory);
    return valid;
}

static bool stream_equals(int fd, const char *expected)
{
    char bytes[128];
    ssize_t amount;
    size_t expected_size;

    if (fd < 0 || expected == NULL) {
        return false;
    }
    expected_size = strlen(expected);
    if (expected_size >= sizeof(bytes)) {
        return false;
    }
    do {
        amount = pread(fd, bytes, sizeof(bytes), 0);
    } while (amount < 0 && errno == EINTR);
    return amount == (ssize_t)expected_size &&
        memcmp(bytes, expected, expected_size) == 0;
}

static bool stream_empty(int fd)
{
    uint8_t byte;
    ssize_t amount;

    do {
        amount = pread(fd, &byte, sizeof(byte), 0);
    } while (amount < 0 && errno == EINTR);
    return amount == 0;
}

static bool named_stream_empty(const char *directory, const char *name)
{
    char path[160];
    int written;
    int fd;
    bool empty;

    written = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return false;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return false;
    }
    empty = stream_empty(fd);
    if (close(fd) != 0) {
        return false;
    }
    return empty;
}

static bool production_ops_are_exact(void)
{
    return
        CHILD_OPS.read_parent_id == child_read_parent_id &&
        CHILD_OPS.arm_parent_death_kill == child_arm_parent_death_kill &&
        CHILD_OPS.isolate_fixed_child_file_descriptors ==
            child_isolate_file_descriptors &&
        CHILD_OPS.reset_and_unblock_child_signals_with_sigpipe_ignored ==
            child_reset_signals &&
        CHILD_OPS.notify_ready_and_close_channel == child_notify_ready &&
        CHILD_OPS.stop_self_until_continued == child_stop_self &&
        CHILD_OPS.run_derived_v2 == child_run_derived_v2 &&
        SUPERVISOR_OPS.inspect_preflight == inspect_preflight &&
        SUPERVISOR_OPS.claim_global_launch_once == claim_global_launch &&
        SUPERVISOR_OPS.claim_evidence_launch_once == claim_evidence_launch &&
        SUPERVISOR_OPS.spawn_one_child == spawn_one_child &&
        SUPERVISOR_OPS.await_child_ready_and_sigstop == await_child_gate &&
        SUPERVISOR_OPS.publish_launch_ready_record == publish_ready_record &&
        SUPERVISOR_OPS.release_gate_and_sample_post_cont_t0 ==
            release_child_gate &&
        SUPERVISOR_OPS.wait_owned_child_until == wait_child_until &&
        SUPERVISOR_OPS.monotonic_now == monotonic_now &&
        SUPERVISOR_OPS.terminate_owned_child_once == terminate_child &&
        SUPERVISOR_OPS.kill_owned_child_once == kill_child &&
        SUPERVISOR_OPS.publish_quarantine_record ==
            publish_quarantine_record &&
        SUPERVISOR_OPS.wait_owned_child_to_reap == wait_child_to_reap &&
        SUPERVISOR_OPS.inspect_postflight == inspect_postflight &&
        SUPERVISOR_OPS.publish_terminal_result == publish_terminal_record;
}

static bool exercise_real_child(
    LinuxSupervisorContext *context,
    SharedChildPidSlot *shared_child_pid
)
{
    const char *test_name = "real-child";
    MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation gate;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorReleaseObservation release;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorWaitObservation wait_result;
    struct sigaction previous_alarm;
    timer_t alarm_timer;
    uint64_t parsed_ticks = 0U;
    uint32_t child_pid = 0U;
    int extra_pipe[2] = {-1, -1};
    int wait_status;
    int spawn_error;
    unsigned int fork_calls_before = supervisor_fork_call_count;
    bool alarm_armed = false;
    bool child_reaped = false;
    bool ok = true;

    TEST_CHECK(test_name, pipe2(extra_pipe, O_CLOEXEC) == 0);
    spawn_error = spawn_one_child(
        context, &context->binding, &child_pid
    );
    if (spawn_error == 0 && child_pid > 0U &&
        child_pid <= (uint32_t)INT32_MAX && shared_child_pid != NULL) {
        shared_child_pid->child_pid = child_pid;
    }
    TEST_CHECK(test_name, spawn_error == 0);
    TEST_CHECK(test_name, child_pid > 0U);
    TEST_CHECK(test_name, child_pid <= (uint32_t)INT32_MAX);
    TEST_CHECK(
        test_name,
        supervisor_fork_call_count == fork_calls_before + 1U
    );
    TEST_CHECK(test_name, context->child_pid == (pid_t)child_pid);
    TEST_CHECK(test_name, arm_gate_alarm(&alarm_timer, &previous_alarm));
    alarm_armed = true;
    memset(&gate, UINT8_C(0xa5), sizeof(gate));
    TEST_CHECK(
        test_name,
        await_child_gate(context, child_pid, &gate) == 0
    );
    if (gate.kind == MEISO_K230D_DRM_SUPERVISOR_GATE_CHILD_EXITED ||
        gate.kind == MEISO_K230D_DRM_SUPERVISOR_GATE_CHILD_SIGNALED) {
        child_reaped = true;
    }
    disarm_gate_alarm(alarm_timer, &previous_alarm);
    alarm_armed = false;
    TEST_CHECK(test_name, monotonic_alarm_seen == 0);
    TEST_CHECK(
        test_name,
        gate.kind == MEISO_K230D_DRM_SUPERVISOR_GATE_READY_AND_STOPPED
    );
    TEST_CHECK(test_name, gate.ready_byte_observed == 1U);
    TEST_CHECK(test_name, gate.exact_wait_child_matched == 1U);
    TEST_CHECK(test_name, gate.child_start_ticks_valid == 1U);
    TEST_CHECK(test_name, gate.child_start_ticks > 0U);
    TEST_CHECK(
        test_name,
        parse_start_ticks((pid_t)child_pid, &parsed_ticks) &&
            parsed_ticks == gate.child_start_ticks
    );
    TEST_CHECK(
        test_name,
        child_descriptor_set_matches((pid_t)child_pid, context)
    );
    memset(&release, UINT8_C(0xa5), sizeof(release));
    TEST_CHECK(
        test_name,
        release_child_gate(context, child_pid, &release) == 0
    );
    TEST_CHECK(
        test_name,
        release.kind ==
            MEISO_K230D_DRM_SUPERVISOR_RELEASED_WITH_POST_CONT_T0
    );
    TEST_CHECK(test_name, release.sigcont_dispatched == 1U);
    TEST_CHECK(test_name, release.t0_valid == 1U);
    TEST_CHECK(
        test_name,
        release.post_cont_t0_ns <= UINT64_MAX - TEST_TIMEOUT_NS
    );
    memset(&wait_result, UINT8_C(0xa5), sizeof(wait_result));
    TEST_CHECK(
        test_name,
        wait_child_until(
            context, child_pid,
            release.post_cont_t0_ns + TEST_TIMEOUT_NS, &wait_result
        ) == 0
    );
    if (wait_result.kind == MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_EXITED ||
        wait_result.kind == MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_SIGNALED) {
        child_reaped = true;
    }
    TEST_CHECK(
        test_name,
        wait_result.kind == MEISO_K230D_DRM_SUPERVISOR_WAIT_CHILD_EXITED
    );
    TEST_CHECK(test_name, wait_result.exit_code_valid == 1U);
    TEST_CHECK(test_name, wait_result.child_exit_code == 0U);
    errno = 0;
    TEST_CHECK(
        test_name,
        waitpid((pid_t)child_pid, &wait_status, WNOHANG) == (pid_t)-1 &&
            errno == ECHILD
    );
    TEST_CHECK(test_name, stream_equals(context->stdout_fd, CHILD_SUCCESS));
    TEST_CHECK(test_name, stream_empty(context->stderr_fd));

cleanup:
    if (alarm_armed) {
        disarm_gate_alarm(alarm_timer, &previous_alarm);
    }
    if (!child_reaped && child_pid > 0U) {
        if (!converge_owned_child((pid_t)child_pid)) {
            fail_at(test_name, __LINE__);
            ok = false;
        }
    }
    if (extra_pipe[0] >= 0) {
        (void)close(extra_pipe[0]);
    }
    if (extra_pipe[1] >= 0) {
        (void)close(extra_pipe[1]);
    }
    return ok;
}

static bool test_forced_fork_failure(void)
{
    const char *test_name = "forced-fork-failure";
    LinuxSupervisorContext context;
    char directory_path[64] = {0};
    uint32_t child_pid = 0U;
    int directory_fd = -1;
    int wait_status = 0;
    int spawn_error;
    unsigned int fork_calls_before = supervisor_fork_call_count;
    bool ok = true;

    TEST_CHECK(
        test_name,
        make_test_directory(directory_path, &directory_fd)
    );
    initialize_context(&context, directory_fd);
    forced_fork_error = EAGAIN;
    spawn_error = spawn_one_child(
        &context, &context.binding, &child_pid
    );
    forced_fork_error = 0;
    TEST_CHECK(test_name, spawn_error == EAGAIN);
    TEST_CHECK(
        test_name,
        supervisor_fork_call_count == fork_calls_before + 1U
    );
    TEST_CHECK(test_name, child_pid == 0U);
    TEST_CHECK(test_name, context.child_pid == (pid_t)-1);
    TEST_CHECK(test_name, context.stdout_fd == -1);
    TEST_CHECK(test_name, context.stderr_fd == -1);
    TEST_CHECK(test_name, context.ready_read_fd == -1);
    TEST_CHECK(test_name, context.ready_write_fd == -1);
    errno = 0;
    TEST_CHECK(
        test_name,
        waitpid((pid_t)-1, &wait_status, WNOHANG) == (pid_t)-1 &&
            errno == ECHILD
    );

cleanup:
    forced_fork_error = 0;
    if (directory_fd >= 0) {
        close_context(&context);
        directory_fd = -1;
    }
    remove_test_directory(directory_path);
    return ok;
}

static bool test_real_child_flow(void)
{
    const char *test_name = "real-child-flow";
    LinuxSupervisorContext context;
    sigset_t previous_mask;
    struct sigaction previous_action;
    char directory_path[64] = {0};
    int directory_fd = -1;
    bool signals_configured = false;
    bool ok = true;

    TEST_CHECK(
        test_name,
        make_test_directory(directory_path, &directory_fd)
    );
    initialize_context(&context, directory_fd);
    TEST_CHECK(
        test_name,
        configure_sigchld_wait(&context, &previous_mask, &previous_action)
    );
    signals_configured = true;
    TEST_CHECK(test_name, exercise_real_child(&context, NULL));

cleanup:
    if (directory_fd >= 0) {
        close_context(&context);
        directory_fd = -1;
    }
    if (signals_configured) {
        restore_sigchld_wait(&previous_mask, &previous_action);
    }
    remove_test_directory(directory_path);
    return ok;
}

static uint8_t run_inherited_sigchld_case(
    const char *directory_path,
    SharedChildPidSlot *shared_child_pid
)
{
    LinuxSupervisorContext context;
    struct sigaction ignored;
    struct sigaction observed;
    sigset_t observed_mask;
    int directory_fd;
    bool valid;

    directory_fd = open(
        directory_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
    );
    if (directory_fd < 0) {
        return 0U;
    }
    initialize_context(&context, directory_fd);
    memset(&ignored, 0, sizeof(ignored));
    ignored.sa_handler = SIG_IGN;
    ignored.sa_flags = SA_NOCLDWAIT;
    if (sigemptyset(&ignored.sa_mask) != 0 ||
        sigaction(SIGCHLD, &ignored, NULL) != 0 ||
        !isolate_supervisor_process(&context) ||
        sigaction(SIGCHLD, NULL, &observed) != 0 ||
        sigprocmask(SIG_BLOCK, NULL, &observed_mask) != 0 ||
        observed.sa_handler != SIG_DFL ||
        (observed.sa_flags & SA_NOCLDWAIT) != 0 ||
        sigismember(&observed_mask, SIGCHLD) != 1) {
        close_context(&context);
        return 0U;
    }
    valid = exercise_real_child(&context, shared_child_pid);
    close_context(&context);
    return valid ? 1U : 0U;
}

static bool test_inherited_sigchld_isolated(void)
{
    const char *test_name = "inherited-sigchld-isolated";
    char directory_path[64] = {0};
    int directory_fd = -1;
    int report_pipe[2] = {-1, -1};
    pid_t case_child = (pid_t)-1;
    uint8_t report = 0U;
    int wait_status = 0;
    SharedChildPidSlot *shared_child_pid = NULL;
    bool case_reaped = false;
    bool ok = true;

    TEST_CHECK(
        test_name,
        make_test_directory(directory_path, &directory_fd)
    );
    TEST_CHECK(test_name, close(directory_fd) == 0);
    directory_fd = -1;
    TEST_CHECK(
        test_name,
        pipe2(report_pipe, O_CLOEXEC | O_NONBLOCK) == 0
    );
    TEST_CHECK(
        test_name,
        map_shared_child_pid_slot(&shared_child_pid)
    );
    case_child = fork();
    TEST_CHECK(test_name, case_child >= 0);
    if (case_child == 0) {
        uint8_t child_report;

        (void)close(report_pipe[0]);
        child_report = run_inherited_sigchld_case(
            directory_path, shared_child_pid
        );
        (void)write_all(report_pipe[1], &child_report, sizeof(child_report));
        (void)close(report_pipe[1]);
        _exit(child_report == 1U ? 0 : 1);
    }
    (void)close(report_pipe[1]);
    report_pipe[1] = -1;
    TEST_CHECK(
        test_name,
        read_all_with_deadline(
            report_pipe[0], &report, sizeof(report), TEST_TIMEOUT_NS * 2U
        )
    );
    TEST_CHECK(
        test_name,
        bounded_reap(case_child, TEST_TIMEOUT_NS * 2U, &wait_status)
    );
    case_reaped = true;
    TEST_CHECK(test_name, WIFEXITED(wait_status));
    TEST_CHECK(test_name, WEXITSTATUS(wait_status) == 0);
    TEST_CHECK(test_name, report == 1U);
    errno = 0;
    TEST_CHECK(
        test_name,
        waitpid((pid_t)-1, &wait_status, WNOHANG) == (pid_t)-1 &&
            errno == ECHILD
    );

cleanup:
    if (!case_reaped && case_child > 0) {
        if (!converge_owned_child(case_child)) {
            fail_at(test_name, __LINE__);
            ok = false;
        } else {
            case_reaped = true;
        }
    }
    if (case_reaped && !converge_shared_child(shared_child_pid)) {
        fail_at(test_name, __LINE__);
        ok = false;
    }
    if (report_pipe[0] >= 0) {
        (void)close(report_pipe[0]);
    }
    if (report_pipe[1] >= 0) {
        (void)close(report_pipe[1]);
    }
    if (directory_fd >= 0) {
        (void)close(directory_fd);
    }
    if (shared_child_pid != NULL &&
        munmap(shared_child_pid, sizeof(*shared_child_pid)) != 0) {
        fail_at(test_name, __LINE__);
        ok = false;
    }
    remove_test_directory(directory_path);
    return ok;
}

static bool run_parent_death_intermediate(
    const char *directory_path,
    int report_fd,
    SharedChildPidSlot *shared_child_pid
)
{
    LinuxSupervisorContext context;
    MeisoK230dDrmSinglePlaneTestOnlySupervisorGateObservation gate;
    struct sigaction previous_alarm;
    sigset_t previous_mask;
    struct sigaction previous_sigchld;
    timer_t alarm_timer;
    ParentDeathPacket packet;
    uint32_t child_pid = 0U;
    int directory_fd = -1;
    bool alarm_armed = false;
    bool signals_configured = false;
    bool child_owned = false;
    bool cleanup_complete = true;
    bool valid = false;
    unsigned int fork_calls_before = supervisor_fork_call_count;
    int spawn_error;

    directory_fd = open(
        directory_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
    );
    if (directory_fd < 0) {
        goto cleanup;
    }
    initialize_context(&context, directory_fd);
    if (!configure_sigchld_wait(
            &context, &previous_mask, &previous_sigchld
        )) {
        goto cleanup;
    }
    signals_configured = true;
    spawn_error = spawn_one_child(
        &context, &context.binding, &child_pid
    );
    if (spawn_error == 0 && child_pid > 0U &&
        child_pid <= (uint32_t)INT32_MAX && shared_child_pid != NULL) {
        shared_child_pid->child_pid = child_pid;
    }
    if (spawn_error != 0 || child_pid == 0U) {
        goto cleanup;
    }
    child_owned = true;
    memset(&packet, 0, sizeof(packet));
    packet.child_pid = child_pid;
    packet.stage = PARENT_DEATH_PACKET_SPAWNED;
    if (!write_all(report_fd, &packet, sizeof(packet))) {
        goto cleanup;
    }
    if (supervisor_fork_call_count != fork_calls_before + 1U) {
        goto cleanup;
    }
    if (!arm_gate_alarm(&alarm_timer, &previous_alarm)) {
        goto cleanup;
    }
    alarm_armed = true;
    memset(&gate, UINT8_C(0xa5), sizeof(gate));
    if (await_child_gate(&context, child_pid, &gate) != 0) {
        goto cleanup;
    }
    disarm_gate_alarm(alarm_timer, &previous_alarm);
    alarm_armed = false;
    if (monotonic_alarm_seen != 0 ||
        gate.kind != MEISO_K230D_DRM_SUPERVISOR_GATE_READY_AND_STOPPED ||
        gate.ready_byte_observed != 1U ||
        gate.exact_wait_child_matched != 1U ||
        gate.child_start_ticks_valid != 1U ||
        gate.child_start_ticks == 0U) {
        goto cleanup;
    }
    memset(&packet, 0, sizeof(packet));
    packet.child_pid = child_pid;
    packet.child_start_ticks = gate.child_start_ticks;
    packet.stage = PARENT_DEATH_PACKET_GATED;
    packet.gate_kind = gate.kind;
    if (!write_all(report_fd, &packet, sizeof(packet))) {
        goto cleanup;
    }
    valid = true;

cleanup:
    if (alarm_armed) {
        disarm_gate_alarm(alarm_timer, &previous_alarm);
    }
    if (!valid && child_owned) {
        cleanup_complete = converge_owned_child((pid_t)child_pid);
        if (!cleanup_complete) {
            fail_at("parent-death-intermediate-cleanup", __LINE__);
        }
        child_owned = false;
    }
    if (directory_fd >= 0) {
        close_context(&context);
    }
    if (signals_configured) {
        restore_sigchld_wait(&previous_mask, &previous_sigchld);
    }
    return valid && cleanup_complete;
}

static bool test_parent_death_kills_stopped_child(void)
{
    const char *test_name = "parent-death-kills-stopped-child";
    char directory_path[64] = {0};
    ParentDeathPacket spawned_packet;
    ParentDeathPacket gated_packet;
    int directory_fd = -1;
    int report_pipe[2] = {-1, -1};
    pid_t intermediate = (pid_t)-1;
    int intermediate_status = 0;
    int child_status = 0;
    SharedChildPidSlot *shared_child_pid = NULL;
    bool intermediate_reaped = false;
    bool child_reaped = false;
    bool ok = true;

    memset(&spawned_packet, 0, sizeof(spawned_packet));
    memset(&gated_packet, 0, sizeof(gated_packet));
    TEST_CHECK(
        test_name,
        make_test_directory(directory_path, &directory_fd)
    );
    TEST_CHECK(test_name, close(directory_fd) == 0);
    directory_fd = -1;
    TEST_CHECK(
        test_name,
        pipe2(report_pipe, O_CLOEXEC | O_NONBLOCK) == 0
    );
    TEST_CHECK(
        test_name,
        map_shared_child_pid_slot(&shared_child_pid)
    );
    intermediate = fork();
    TEST_CHECK(test_name, intermediate >= 0);
    if (intermediate == 0) {
        bool child_ready;

        (void)close(report_pipe[0]);
        child_ready = run_parent_death_intermediate(
            directory_path, report_pipe[1], shared_child_pid
        );
        (void)close(report_pipe[1]);
        _exit(child_ready ? 0 : 1);
    }
    (void)close(report_pipe[1]);
    report_pipe[1] = -1;
    TEST_CHECK(
        test_name,
        read_all_with_deadline(
            report_pipe[0], &spawned_packet, sizeof(spawned_packet),
            TEST_TIMEOUT_NS * 2U
        )
    );
    TEST_CHECK(
        test_name,
        spawned_packet.stage == PARENT_DEATH_PACKET_SPAWNED
    );
    TEST_CHECK(test_name, spawned_packet.child_pid > 0U);
    TEST_CHECK(test_name, spawned_packet.child_start_ticks == 0U);
    TEST_CHECK(test_name, spawned_packet.gate_kind == 0U);
    TEST_CHECK(
        test_name,
        read_all_with_deadline(
            report_pipe[0], &gated_packet, sizeof(gated_packet),
            TEST_TIMEOUT_NS * 2U
        )
    );
    TEST_CHECK(
        test_name,
        gated_packet.stage == PARENT_DEATH_PACKET_GATED
    );
    TEST_CHECK(
        test_name,
        gated_packet.child_pid == spawned_packet.child_pid
    );
    TEST_CHECK(
        test_name,
        gated_packet.gate_kind ==
            MEISO_K230D_DRM_SUPERVISOR_GATE_READY_AND_STOPPED
    );
    TEST_CHECK(test_name, gated_packet.child_start_ticks > 0U);
    TEST_CHECK(
        test_name,
        bounded_reap(intermediate, TEST_TIMEOUT_NS, &intermediate_status)
    );
    intermediate_reaped = true;
    TEST_CHECK(test_name, WIFEXITED(intermediate_status));
    TEST_CHECK(test_name, WEXITSTATUS(intermediate_status) == 0);
    TEST_CHECK(
        test_name,
        bounded_reap(
            (pid_t)spawned_packet.child_pid,
            TEST_TIMEOUT_NS,
            &child_status
        )
    );
    child_reaped = true;
    TEST_CHECK(test_name, WIFSIGNALED(child_status));
    TEST_CHECK(test_name, WTERMSIG(child_status) == SIGKILL);
    TEST_CHECK(
        test_name,
        named_stream_empty(directory_path, STDOUT_RECORD)
    );
    TEST_CHECK(
        test_name,
        named_stream_empty(directory_path, STDERR_RECORD)
    );
    errno = 0;
    TEST_CHECK(
        test_name,
        waitpid(
            (pid_t)spawned_packet.child_pid, &child_status, WNOHANG
        ) ==
                (pid_t)-1 &&
            errno == ECHILD
    );

cleanup:
    if (!intermediate_reaped && intermediate > 0) {
        if (!converge_owned_child(intermediate)) {
            fail_at(test_name, __LINE__);
            ok = false;
        } else {
            intermediate_reaped = true;
        }
    }
    if (!child_reaped && intermediate_reaped &&
        !converge_shared_child(shared_child_pid)) {
        fail_at(test_name, __LINE__);
        ok = false;
    }
    if (report_pipe[0] >= 0) {
        (void)close(report_pipe[0]);
    }
    if (report_pipe[1] >= 0) {
        (void)close(report_pipe[1]);
    }
    if (directory_fd >= 0) {
        (void)close(directory_fd);
    }
    if (shared_child_pid != NULL &&
        munmap(shared_child_pid, sizeof(*shared_child_pid)) != 0) {
        fail_at(test_name, __LINE__);
        ok = false;
    }
    remove_test_directory(directory_path);
    return ok;
}

static bool string_equals(const char *value, const char *expected)
{
    return value != NULL && expected != NULL && strcmp(value, expected) == 0;
}

static bool child_signal_state_is_exact(void)
{
    static const int DEFAULT_SIGNALS[] = {
        SIGHUP, SIGINT, SIGTERM, SIGCHLD, SIGALRM
    };
    sigset_t mask;
    struct sigaction action;
    size_t index;
    int signal_number;

    if (sigprocmask(SIG_BLOCK, NULL, &mask) != 0) {
        return false;
    }
    for (signal_number = 1; signal_number < NSIG; ++signal_number) {
        if (sigismember(&mask, signal_number) != 0) {
            return false;
        }
    }
    if (sigaction(SIGPIPE, NULL, &action) != 0 ||
        action.sa_handler != SIG_IGN) {
        return false;
    }
    for (index = 0U;
         index < sizeof(DEFAULT_SIGNALS) / sizeof(DEFAULT_SIGNALS[0]);
         ++index) {
        if (sigaction(DEFAULT_SIGNALS[index], NULL, &action) != 0 ||
            action.sa_handler != SIG_DFL) {
            return false;
        }
    }
    return true;
}

static bool child_stdio_is_exact(FILE *out_stream, FILE *error_stream)
{
    struct stat stdin_stat;
    struct stat stdout_stat;
    struct stat stderr_stat;
    struct stat null_stat;

    return out_stream != NULL && error_stream != NULL &&
        fileno(out_stream) == STDOUT_FILENO &&
        fileno(error_stream) == STDERR_FILENO &&
        fstat(STDIN_FILENO, &stdin_stat) == 0 &&
        fstat(STDOUT_FILENO, &stdout_stat) == 0 &&
        fstat(STDERR_FILENO, &stderr_stat) == 0 &&
        stat("/dev/null", &null_stat) == 0 &&
        S_ISCHR(stdin_stat.st_mode) &&
        stdin_stat.st_rdev == null_stat.st_rdev &&
        S_ISREG(stdout_stat.st_mode) && S_ISREG(stderr_stat.st_mode) &&
        (stdout_stat.st_dev != stderr_stat.st_dev ||
         stdout_stat.st_ino != stderr_stat.st_ino);
}

static int meiso_test_derived_once(
    FILE *out_stream,
    FILE *error_stream,
    int argument_count,
    char **arguments,
    MeisoK230dDrmNativeSinglePlaneTestOnlyDeriveUniquePlaneExecuteFn execute
)
{
    bool valid = argument_count == 10 && arguments != NULL &&
        string_equals(arguments[0], "meiso-supervised-direct-derived-v2") &&
        string_equals(arguments[1], "--derive-unique-plane") &&
        string_equals(arguments[2], "--device") &&
        string_equals(arguments[3], "/dev/dri/card0") &&
        string_equals(arguments[4], "--topology-sha256") &&
        string_equals(arguments[5], TOPOLOGY_HEX) &&
        string_equals(arguments[6], "--connector-id") &&
        string_equals(arguments[7], "37") &&
        string_equals(arguments[8], "--crtc-id") &&
        string_equals(arguments[9], "46") &&
        execute == meiso_test_native_derived_provider &&
        child_stdio_is_exact(out_stream, error_stream) &&
        child_signal_state_is_exact();

    if (!valid) {
        if (error_stream != NULL) {
            (void)fputs("HARmless-child-contract-failure\n", error_stream);
            (void)fflush(error_stream);
        }
        return 1;
    }
    if (fputs(CHILD_SUCCESS, out_stream) == EOF ||
        fflush(out_stream) != 0) {
        return 1;
    }
    return 0;
}

static MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus
meiso_test_native_derived_provider(
    const char *device_path,
    const MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest *request,
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult *out_result
)
{
    (void)device_path;
    (void)request;
    (void)out_result;
    _exit(TEST_PROVIDER_CALLED_EXIT);
}

static pid_t meiso_test_supervisor_fork(void)
{
    ++supervisor_fork_call_count;
    if (forced_fork_error != 0) {
        errno = forced_fork_error;
        return (pid_t)-1;
    }
    return fork();
}

int main(void)
{
    int previous_subreaper = 0;
    bool subreaper_available = false;
    bool subreaper_changed = false;
    bool ok = true;

    if (!production_ops_are_exact()) {
        fail_at("production-ops-wiring", __LINE__);
        ok = false;
    }
    if (ok && !test_forced_fork_failure()) {
        ok = false;
    }
    if (ok && prctl(PR_GET_CHILD_SUBREAPER, &previous_subreaper) != 0) {
        fail_at("subreaper-query", __LINE__);
        return 1;
    }
    if (ok && previous_subreaper == 0) {
        if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) {
            fail_at("subreaper-enable", __LINE__);
            return 1;
        }
        subreaper_changed = true;
    }
    if (ok) {
        subreaper_available = true;
    }
    if (ok && !test_real_child_flow()) {
        ok = false;
    }
    if (ok && !test_inherited_sigchld_isolated()) {
        ok = false;
    }
    if (ok && !test_parent_death_kills_stopped_child()) {
        ok = false;
    }
    if (subreaper_available && !global_child_empty_fence()) {
        fail_at("final-child-empty-fence", __LINE__);
        ok = false;
    }
    if (subreaper_changed && prctl(PR_SET_CHILD_SUBREAPER, 0) != 0) {
        fail_at("subreaper-restore", __LINE__);
        ok = false;
    }
    return ok ? 0 : 1;
}
