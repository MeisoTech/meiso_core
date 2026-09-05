#define _POSIX_C_SOURCE 200809L

#include "display_ingress_simulated_service_private.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    PAIR_CHILD_OUTPUT_CAPACITY = 2048,
    PAIR_AGGREGATE_CAPACITY = 8192,
    PAIR_TIMEOUT_MS = 10000,
    PAIR_POLL_SLICE_MS = 20,
    PAIR_LOOPBACK_ADDRESS_HOST_ORDER = 0x7f000001
};

typedef struct {
    pid_t pid;
    int stdout_fd;
    int stderr_fd;
    int exit_code;
    bool reaped;
    size_t stdout_size;
    size_t stderr_size;
    char stdout_bytes[PAIR_CHILD_OUTPUT_CAPACITY];
    char stderr_bytes[PAIR_CHILD_OUTPUT_CAPACITY];
} PairChild;

typedef struct {
    char session_id[65U];
    char frame_sha256[65U];
    char evidence_sha256[65U];
    uint64_t catalog_generation;
    uint64_t applied_desired_version;
    uint64_t actual_version;
    uint64_t production_epoch;
    uint64_t media_stream_epoch;
    uint64_t frame_sequence;
    uint64_t source_clock_epoch;
    uint64_t source_time_ns;
    uint64_t sender_deadline_clock_epoch;
    uint64_t sender_deadline_ns;
    uint64_t producer_dropped_frames;
    uint32_t input_width;
    uint32_t input_height;
    uint32_t row_pitch_bytes;
    uint32_t rgba_bytes;
} PairRoleReceipt;

typedef enum {
    PAIR_STAGE_INPUT,
    PAIR_STAGE_BOOTSTRAP,
    PAIR_STAGE_CHILD,
    PAIR_STAGE_RECEIPT,
    PAIR_STAGE_TIMEOUT
} PairFailureStage;

static void pair_close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static bool pair_set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL);

    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool pair_set_close_on_exec(int fd)
{
    const int flags = fcntl(fd, F_GETFD);

    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static bool pair_fd_is_open(int fd)
{
    int flags;

    do {
        flags = fcntl(fd, F_GETFD);
    } while (flags < 0 && errno == EINTR);
    return flags >= 0;
}

static uint64_t pair_now_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) {
        return 0U;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000) +
        (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static bool pair_prepare_signals(void)
{
    struct sigaction child_action;
    struct sigaction pipe_action;

    memset(&child_action, 0, sizeof(child_action));
    memset(&pipe_action, 0, sizeof(pipe_action));
    child_action.sa_handler = SIG_DFL;
    pipe_action.sa_handler = SIG_IGN;
    return sigemptyset(&child_action.sa_mask) == 0 &&
        sigemptyset(&pipe_action.sa_mask) == 0 &&
        sigaction(SIGCHLD, &child_action, NULL) == 0 &&
        sigaction(SIGPIPE, &pipe_action, NULL) == 0;
}

static int pair_poll_timeout(uint64_t deadline_ms)
{
    const uint64_t now = pair_now_ms();
    uint64_t remaining;

    if (now == 0U || now >= deadline_ms) {
        return 0;
    }
    remaining = deadline_ms - now;
    return remaining < PAIR_POLL_SLICE_MS ?
        (int)remaining : PAIR_POLL_SLICE_MS;
}

static void pair_child_initialize(PairChild *child)
{
    if (child != NULL) {
        memset(child, 0, sizeof(*child));
        child->pid = -1;
        child->stdout_fd = -1;
        child->stderr_fd = -1;
        child->exit_code = -1;
    }
}

static bool pair_make_pipe(int descriptors[2], bool nonblocking_reader)
{
    descriptors[0] = -1;
    descriptors[1] = -1;
    if (pipe(descriptors) != 0 ||
        !pair_set_close_on_exec(descriptors[0]) ||
        !pair_set_close_on_exec(descriptors[1]) ||
        (nonblocking_reader && !pair_set_nonblocking(descriptors[0]))) {
        pair_close_fd(&descriptors[0]);
        pair_close_fd(&descriptors[1]);
        return false;
    }
    return true;
}

static bool pair_make_tcp_pair(int descriptors[2])
{
    struct sockaddr_in address;
    socklen_t address_size = (socklen_t)sizeof(address);
    int listener = -1;
    int client = -1;
    int server = -1;
    bool ok = false;

    descriptors[0] = -1;
    descriptors[1] = -1;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        goto done;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(PAIR_LOOPBACK_ADDRESS_HOST_ORDER);
    if (bind(
            listener,
            (const struct sockaddr *)(const void *)&address,
            sizeof(address)
        ) != 0 || listen(listener, 1) != 0 || getsockname(
            listener, (struct sockaddr *)(void *)&address, &address_size
        ) != 0) {
        goto done;
    }
    client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0 || connect(
            client,
            (const struct sockaddr *)(const void *)&address,
            address_size
        ) != 0) {
        goto done;
    }
    server = accept(listener, NULL, NULL);
    if (server < 0 || !pair_set_nonblocking(client) ||
        !pair_set_nonblocking(server) ||
        !pair_set_close_on_exec(client) ||
        !pair_set_close_on_exec(server)) {
        goto done;
    }
    descriptors[0] = client;
    descriptors[1] = server;
    client = -1;
    server = -1;
    ok = true;

done:
    pair_close_fd(&listener);
    pair_close_fd(&client);
    pair_close_fd(&server);
    return ok;
}

static bool pair_bind_udp_loopback(int fd, struct sockaddr_in *out_address)
{
    struct sockaddr_in address;
    socklen_t address_size = (socklen_t)sizeof(address);

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(PAIR_LOOPBACK_ADDRESS_HOST_ORDER);
    if (bind(
            fd,
            (const struct sockaddr *)(const void *)&address,
            sizeof(address)
        ) != 0 || getsockname(
            fd, (struct sockaddr *)(void *)&address, &address_size
        ) != 0) {
        return false;
    }
    *out_address = address;
    return true;
}

static bool pair_make_udp_pair(int descriptors[2])
{
    struct sockaddr_in first;
    struct sockaddr_in second;

    descriptors[0] = socket(AF_INET, SOCK_DGRAM, 0);
    descriptors[1] = socket(AF_INET, SOCK_DGRAM, 0);
    if (descriptors[0] < 0 || descriptors[1] < 0 ||
        !pair_bind_udp_loopback(descriptors[0], &first) ||
        !pair_bind_udp_loopback(descriptors[1], &second) ||
        connect(
            descriptors[0],
            (const struct sockaddr *)(const void *)&second,
            sizeof(second)
        ) != 0 || connect(
            descriptors[1],
            (const struct sockaddr *)(const void *)&first,
            sizeof(first)
        ) != 0 || !pair_set_nonblocking(descriptors[0]) ||
        !pair_set_nonblocking(descriptors[1]) ||
        !pair_set_close_on_exec(descriptors[0]) ||
        !pair_set_close_on_exec(descriptors[1])) {
        pair_close_fd(&descriptors[0]);
        pair_close_fd(&descriptors[1]);
        return false;
    }
    return true;
}

static int pair_duplicate_fd(int fd)
{
    int duplicate;

    do {
        duplicate = fcntl(fd, F_DUPFD, 7);
    } while (duplicate < 0 && errno == EINTR);
    return duplicate;
}

static void pair_close_child_descriptor_range(void)
{
    struct rlimit limit;
    rlim_t maximum = 1048576U;
    int fd;

    if (getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        if (limit.rlim_max != RLIM_INFINITY) {
            maximum = limit.rlim_max;
        } else if (limit.rlim_cur != RLIM_INFINITY &&
                   limit.rlim_cur > maximum) {
            maximum = limit.rlim_cur;
        }
    }
    if (maximum > (rlim_t)INT_MAX) {
        maximum = (rlim_t)INT_MAX;
    }
    for (fd = 7; (rlim_t)fd < maximum; ++fd) {
        (void)close(fd);
    }
}

static bool pair_prepare_child_descriptors(
    int stdout_fd,
    int stderr_fd,
    int control_fd,
    int media_fd,
    int frame_fd,
    int readiness_fd
)
{
    int copies[6U] = {-1, -1, -1, -1, -1, -1};
    const int sources[6U] = {
        stdout_fd, stderr_fd, control_fd, media_fd, frame_fd, readiness_fd
    };
    const int targets[6U] = {
        STDOUT_FILENO, STDERR_FILENO, 3, 4, 5, 6
    };
    size_t index;
    bool ok = true;

    for (index = 0U; index < 6U; ++index) {
        if (sources[index] >= 0) {
            copies[index] = pair_duplicate_fd(sources[index]);
            if (copies[index] < 0) {
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        for (index = 0U; index < 6U; ++index) {
            if (copies[index] >= 0 &&
                dup2(copies[index], targets[index]) < 0) {
                ok = false;
                break;
            }
        }
    }
    if (!ok) {
        for (index = 0U; index < 6U; ++index) {
            pair_close_fd(&copies[index]);
        }
        return false;
    }

    (void)close(STDIN_FILENO);
    pair_close_child_descriptor_range();
    if (frame_fd < 0) {
        (void)close(5);
    }
    if (readiness_fd < 0) {
        (void)close(6);
    }
    clearerr(stdout);
    clearerr(stderr);
    return true;
}

static bool pair_spawn_role(
    MeisoSessionRole role,
    int control_fd,
    int media_fd,
    int frame_fd,
    int readiness_fd,
    PairChild *out_child
)
{
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    pid_t pid;

    if (out_child == NULL || control_fd < 0 || media_fd < 0 ||
        (role == MEISO_SESSION_ROLE_HOST &&
         (frame_fd < 0 || readiness_fd < 0)) ||
        (role == MEISO_SESSION_ROLE_EDGE &&
         (frame_fd >= 0 || readiness_fd >= 0)) ||
        !pair_make_pipe(stdout_pipe, true) ||
        !pair_make_pipe(stderr_pipe, true) || fflush(NULL) != 0) {
        pair_close_fd(&stdout_pipe[0]);
        pair_close_fd(&stdout_pipe[1]);
        pair_close_fd(&stderr_pipe[0]);
        pair_close_fd(&stderr_pipe[1]);
        return false;
    }

    pid = fork();
    if (pid < 0) {
        pair_close_fd(&stdout_pipe[0]);
        pair_close_fd(&stdout_pipe[1]);
        pair_close_fd(&stderr_pipe[0]);
        pair_close_fd(&stderr_pipe[1]);
        return false;
    }
    if (pid == 0) {
        int result;

        if (!pair_prepare_child_descriptors(
                stdout_pipe[1],
                stderr_pipe[1],
                control_fd,
                media_fd,
                frame_fd,
                readiness_fd
            )) {
            _exit(126);
        }
        result = service_execute_role(
            role,
            role == MEISO_SESSION_ROLE_HOST ? "host" : "edge",
            3,
            4,
            role == MEISO_SESSION_ROLE_HOST ? 5 : -1,
            role == MEISO_SESSION_ROLE_HOST ? 6 : -1
        );
        if (fflush(stdout) != 0 || fflush(stderr) != 0) {
            result = 1;
        }
        _exit(result);
    }

    pair_close_fd(&stdout_pipe[1]);
    pair_close_fd(&stderr_pipe[1]);
    pair_child_initialize(out_child);
    out_child->pid = pid;
    out_child->stdout_fd = stdout_pipe[0];
    out_child->stderr_fd = stderr_pipe[0];
    return true;
}

static bool pair_drain_output(
    int *fd,
    char output[PAIR_CHILD_OUTPUT_CAPACITY],
    size_t *used
)
{
    if (fd == NULL || output == NULL || used == NULL ||
        *used >= PAIR_CHILD_OUTPUT_CAPACITY) {
        return false;
    }
    while (*fd >= 0 && *used + 1U < PAIR_CHILD_OUTPUT_CAPACITY) {
        const ssize_t count = read(
            *fd,
            output + *used,
            PAIR_CHILD_OUTPUT_CAPACITY - *used - 1U
        );

        if (count > 0) {
            *used += (size_t)count;
            output[*used] = '\0';
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 &&
                   (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        } else if (count == 0) {
            pair_close_fd(fd);
            return true;
        } else {
            return false;
        }
    }
    return *used + 1U < PAIR_CHILD_OUTPUT_CAPACITY;
}

static bool pair_update_child(PairChild *child)
{
    int status;
    pid_t waited;

    if (child == NULL || child->pid <= 0 ||
        !pair_drain_output(
            &child->stdout_fd, child->stdout_bytes, &child->stdout_size
        ) || !pair_drain_output(
            &child->stderr_fd, child->stderr_bytes, &child->stderr_size
        )) {
        return false;
    }
    if (child->reaped) {
        return true;
    }
    waited = waitpid(child->pid, &status, WNOHANG);
    if (waited == child->pid) {
        child->reaped = true;
        child->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    } else if (waited < 0 && errno != EINTR) {
        return false;
    }
    return true;
}

static void pair_terminate_children(PairChild *children, size_t count)
{
    size_t index;

    if (children == NULL) {
        return;
    }
    for (index = 0U; index < count; ++index) {
        if (children[index].pid > 0 && !children[index].reaped) {
            (void)kill(children[index].pid, SIGKILL);
        }
    }
    for (index = 0U; index < count; ++index) {
        if (children[index].pid > 0 && !children[index].reaped) {
            while (waitpid(children[index].pid, NULL, 0) < 0 &&
                   errno == EINTR) {
            }
            children[index].reaped = true;
            children[index].exit_code = -1;
        }
        pair_close_fd(&children[index].stdout_fd);
        pair_close_fd(&children[index].stderr_fd);
    }
}

typedef enum {
    PAIR_COLLECT_OK,
    PAIR_COLLECT_FAILED,
    PAIR_COLLECT_TIMEOUT
} PairCollectResult;

static PairCollectResult pair_collect_children(
    PairChild *children,
    size_t count,
    uint64_t deadline_ms
)
{
    while (pair_now_ms() != 0U && pair_now_ms() < deadline_ms) {
        struct pollfd descriptors[4U];
        nfds_t descriptor_count = 0U;
        bool all_reaped = true;
        bool all_closed = true;
        size_t index;

        for (index = 0U; index < count; ++index) {
            if (!pair_update_child(&children[index])) {
                pair_terminate_children(children, count);
                return PAIR_COLLECT_FAILED;
            }
            all_reaped = all_reaped && children[index].reaped;
            all_closed = all_closed && children[index].stdout_fd < 0 &&
                children[index].stderr_fd < 0;
            if (children[index].stdout_fd >= 0) {
                descriptors[descriptor_count++] = (struct pollfd){
                    children[index].stdout_fd, POLLIN, 0
                };
            }
            if (children[index].stderr_fd >= 0) {
                descriptors[descriptor_count++] = (struct pollfd){
                    children[index].stderr_fd, POLLIN, 0
                };
            }
        }
        if (all_reaped && all_closed) {
            return PAIR_COLLECT_OK;
        }
        if (poll(
                descriptors,
                descriptor_count,
                pair_poll_timeout(deadline_ms)
            ) < 0 && errno != EINTR) {
            pair_terminate_children(children, count);
            return PAIR_COLLECT_FAILED;
        }
    }
    pair_terminate_children(children, count);
    return PAIR_COLLECT_TIMEOUT;
}

typedef enum {
    PAIR_READY_OK,
    PAIR_READY_EMPTY,
    PAIR_READY_INVALID,
    PAIR_READY_FAILED,
    PAIR_READY_TIMEOUT
} PairReadyResult;

static PairReadyResult pair_wait_for_host_ready(
    PairChild *host,
    int *readiness_fd,
    uint64_t deadline_ms
)
{
    uint8_t bytes[2U];
    size_t used = 0U;

    while (pair_now_ms() != 0U && pair_now_ms() < deadline_ms) {
        struct pollfd descriptors[3U];
        nfds_t descriptor_count = 0U;
        bool readiness_eof = false;

        if (!pair_update_child(host)) {
            pair_close_fd(readiness_fd);
            return PAIR_READY_FAILED;
        }
        while (*readiness_fd >= 0 && used < sizeof(bytes)) {
            const ssize_t count = read(
                *readiness_fd, bytes + used, sizeof(bytes) - used
            );

            if (count > 0) {
                used += (size_t)count;
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else if (count < 0 &&
                       (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else if (count == 0) {
                readiness_eof = true;
                pair_close_fd(readiness_fd);
                break;
            } else {
                pair_close_fd(readiness_fd);
                return PAIR_READY_FAILED;
            }
        }
        if (used == sizeof(bytes)) {
            pair_close_fd(readiness_fd);
            return PAIR_READY_INVALID;
        }
        if (readiness_eof) {
            if (used == 0U) {
                return PAIR_READY_EMPTY;
            }
            if (used == 1U &&
                bytes[0] == (uint8_t)SERVICE_PAIR_INPUT_READY_BYTE &&
                !host->reaped && host->stdout_size == 0U &&
                host->stderr_size == 0U) {
                return PAIR_READY_OK;
            }
            return PAIR_READY_INVALID;
        }
        if (host->reaped) {
            pair_close_fd(readiness_fd);
            return used == 0U ? PAIR_READY_EMPTY : PAIR_READY_INVALID;
        }
        if (*readiness_fd >= 0) {
            descriptors[descriptor_count++] = (struct pollfd){
                *readiness_fd, POLLIN, 0
            };
        }
        if (host->stdout_fd >= 0) {
            descriptors[descriptor_count++] = (struct pollfd){
                host->stdout_fd, POLLIN, 0
            };
        }
        if (host->stderr_fd >= 0) {
            descriptors[descriptor_count++] = (struct pollfd){
                host->stderr_fd, POLLIN, 0
            };
        }
        if (poll(
                descriptors,
                descriptor_count,
                pair_poll_timeout(deadline_ms)
            ) < 0 && errno != EINTR) {
            pair_close_fd(readiness_fd);
            return PAIR_READY_FAILED;
        }
    }
    pair_close_fd(readiness_fd);
    return PAIR_READY_TIMEOUT;
}

static bool pair_consume_literal(
    const char **cursor,
    const char *end,
    const char *literal
)
{
    size_t size;

    if (cursor == NULL || *cursor == NULL || end == NULL ||
        literal == NULL || *cursor > end) {
        return false;
    }
    size = strlen(literal);
    if ((size_t)(end - *cursor) < size ||
        memcmp(*cursor, literal, size) != 0) {
        return false;
    }
    *cursor += size;
    return true;
}

static bool pair_hex_segment_nonzero(const char *value, size_t size)
{
    size_t index;

    if (value == NULL) {
        return false;
    }
    for (index = 0U; index < size; ++index) {
        if (value[index] != '0') {
            return true;
        }
    }
    return false;
}

static bool pair_parse_u64(
    const char **cursor,
    const char *end,
    uint64_t *out_value
)
{
    const char *position;
    uint64_t value = 0U;

    if (cursor == NULL || *cursor == NULL || out_value == NULL ||
        *cursor >= end || **cursor < '0' || **cursor > '9') {
        return false;
    }
    position = *cursor;
    if (*position == '0' && position + 1 < end &&
        position[1] >= '0' && position[1] <= '9') {
        return false;
    }
    while (position < end && *position >= '0' && *position <= '9') {
        const uint64_t digit = (uint64_t)(*position - '0');

        if (value > (UINT64_MAX - digit) / UINT64_C(10)) {
            return false;
        }
        value = value * UINT64_C(10) + digit;
        ++position;
    }
    *cursor = position;
    *out_value = value;
    return true;
}

static bool pair_parse_u32(
    const char **cursor,
    const char *end,
    uint32_t *out_value
)
{
    uint64_t value;

    if (!pair_parse_u64(cursor, end, &value) || value > UINT32_MAX) {
        return false;
    }
    *out_value = (uint32_t)value;
    return true;
}

static bool pair_parse_hex64(
    const char **cursor,
    const char *end,
    char output[65U]
)
{
    size_t index;

    if (cursor == NULL || *cursor == NULL || output == NULL ||
        *cursor > end || (size_t)(end - *cursor) < 64U) {
        return false;
    }
    for (index = 0U; index < 64U; ++index) {
        const char value = (*cursor)[index];

        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            return false;
        }
        output[index] = value;
    }
    output[64U] = '\0';
    *cursor += 64U;
    return true;
}

static bool pair_parse_u64_field(
    const char **cursor,
    const char *end,
    const char *prefix,
    uint64_t *out_value
)
{
    return pair_consume_literal(cursor, end, prefix) &&
        pair_parse_u64(cursor, end, out_value);
}

static bool pair_parse_u32_field(
    const char **cursor,
    const char *end,
    const char *prefix,
    uint32_t *out_value
)
{
    return pair_consume_literal(cursor, end, prefix) &&
        pair_parse_u32(cursor, end, out_value);
}

static bool pair_parse_role_receipt(
    const PairChild *child,
    bool host,
    PairRoleReceipt *out_receipt
)
{
    PairRoleReceipt value;
    const char *cursor;
    const char *end;

    if (child == NULL || out_receipt == NULL || child->exit_code != 0 ||
        child->stderr_size != 0U || child->stdout_size == 0U ||
        child->stdout_size >= PAIR_CHILD_OUTPUT_CAPACITY ||
        strlen(child->stdout_bytes) != child->stdout_size) {
        return false;
    }
    memset(&value, 0, sizeof(value));
    cursor = child->stdout_bytes;
    end = child->stdout_bytes + child->stdout_size;
    if (!pair_consume_literal(
            &cursor,
            end,
            host ?
                "{\"schema\":\"DISPLAY_INGRESS_SIMULATED_SERVICE_RECEIPT_V2\","
                "\"role\":\"host\",\"status\":\"SUCCESS\","
                "\"frame_input\":\"DIFI/v1\",\"session_id\":\"" :
                "{\"schema\":\"DISPLAY_INGRESS_SIMULATED_SERVICE_RECEIPT_V2\","
                "\"role\":\"edge\",\"status\":\"SUCCESS\","
                "\"session_id\":\""
        ) || !pair_parse_hex64(&cursor, end, value.session_id) ||
        !pair_parse_u64_field(
            &cursor, end, "\",\"catalog_generation\":",
            &value.catalog_generation
        ) || !pair_parse_u64_field(
            &cursor, end, ",\"applied_desired_version\":",
            &value.applied_desired_version
        ) || !pair_parse_u64_field(
            &cursor, end, ",\"actual_version\":", &value.actual_version
        ) || !pair_parse_u64_field(
            &cursor, end, ",\"production_epoch\":",
            &value.production_epoch
        ) || !pair_parse_u64_field(
            &cursor, end, ",\"media_stream_epoch\":",
            &value.media_stream_epoch
        ) || !pair_parse_u32_field(
            &cursor, end, ",\"input_width\":", &value.input_width
        ) || !pair_parse_u32_field(
            &cursor, end, ",\"input_height\":", &value.input_height
        ) || !pair_parse_u32_field(
            &cursor, end, ",\"row_pitch_bytes\":", &value.row_pitch_bytes
        ) || !pair_parse_u32_field(
            &cursor, end, ",\"rgba_bytes\":", &value.rgba_bytes
        ) || !pair_parse_u64_field(
            &cursor, end, ",\"frame_sequence\":", &value.frame_sequence
        ) || !pair_parse_u64_field(
            &cursor, end, ",\"source_clock_epoch\":",
            &value.source_clock_epoch
        ) || !pair_parse_u64_field(
            &cursor, end, ",\"source_time_ns\":", &value.source_time_ns
        ) || !pair_consume_literal(
            &cursor, end, ",\"sender_deadline_present\":false"
        ) || !pair_parse_u64_field(
            &cursor, end, ",\"sender_deadline_clock_epoch\":",
            &value.sender_deadline_clock_epoch
        ) || !pair_parse_u64_field(
            &cursor, end, ",\"sender_deadline_ns\":",
            &value.sender_deadline_ns
        ) || !pair_parse_u64_field(
            &cursor, end, ",\"producer_dropped_frames\":",
            &value.producer_dropped_frames
        ) || !pair_consume_literal(
            &cursor,
            end,
            host ? ",\"input_sha256\":\"" : ",\"received_sha256\":\""
        ) || !pair_parse_hex64(&cursor, end, value.frame_sha256) ||
        !pair_consume_literal(
            &cursor, end, "\",\"evidence_input_sha256\":\""
        ) || !pair_parse_hex64(&cursor, end, value.evidence_sha256) ||
        !pair_consume_literal(
            &cursor,
            end,
            host ?
                "\",\"evidence_class\":\"SIMULATED\","
                "\"connector\":\"UNSPECIFIED\","
                "\"carrier\":\"CONTROLLED_RAW_UDP\","
                "\"physical_evidence\":false,\"link_security\":false,"
                "\"clean_shutdown\":true}\n" :
                "\",\"evidence_class\":\"SIMULATED\","
                "\"connector\":\"UNSPECIFIED\","
                "\"udp_armed_before_actual_write\":true,"
                "\"carrier\":\"CONTROLLED_RAW_UDP\","
                "\"physical_evidence\":false,\"link_security\":false,"
                "\"clean_shutdown\":true}\n"
        ) || cursor != end ||
        !pair_hex_segment_nonzero(value.session_id, 32U) ||
        !pair_hex_segment_nonzero(value.session_id + 32U, 32U) ||
        value.catalog_generation != 1U ||
        value.applied_desired_version != 1U || value.actual_version != 1U ||
        value.production_epoch != 1U || value.media_stream_epoch != 1U ||
        value.input_width != SERVICE_FRAME_WIDTH ||
        value.input_height != SERVICE_FRAME_HEIGHT ||
        value.row_pitch_bytes != SERVICE_FRAME_WIDTH * 4U ||
        value.rgba_bytes != SERVICE_FRAME_BYTES ||
        value.frame_sequence != 1U || value.source_clock_epoch == 0U ||
        value.sender_deadline_clock_epoch != 0U ||
        value.sender_deadline_ns != 0U || strcmp(
            value.frame_sha256, value.evidence_sha256
        ) != 0) {
        return false;
    }
    *out_receipt = value;
    return true;
}

static bool pair_role_receipts_match(
    const PairRoleReceipt *host,
    const PairRoleReceipt *edge
)
{
    return host != NULL && edge != NULL &&
        strcmp(host->session_id, edge->session_id) == 0 &&
        strcmp(host->frame_sha256, edge->frame_sha256) == 0 &&
        strcmp(host->evidence_sha256, edge->evidence_sha256) == 0 &&
        host->catalog_generation == edge->catalog_generation &&
        host->applied_desired_version == edge->applied_desired_version &&
        host->actual_version == edge->actual_version &&
        host->production_epoch == edge->production_epoch &&
        host->media_stream_epoch == edge->media_stream_epoch &&
        host->frame_sequence == edge->frame_sequence &&
        host->source_clock_epoch == edge->source_clock_epoch &&
        host->source_time_ns == edge->source_time_ns &&
        host->sender_deadline_clock_epoch ==
            edge->sender_deadline_clock_epoch &&
        host->sender_deadline_ns == edge->sender_deadline_ns &&
        host->producer_dropped_frames == edge->producer_dropped_frames &&
        host->input_width == edge->input_width &&
        host->input_height == edge->input_height &&
        host->row_pitch_bytes == edge->row_pitch_bytes &&
        host->rgba_bytes == edge->rgba_bytes;
}

static const char *pair_failure_stage_name(PairFailureStage stage)
{
    switch (stage) {
        case PAIR_STAGE_INPUT:
            return "input";
        case PAIR_STAGE_BOOTSTRAP:
            return "bootstrap";
        case PAIR_STAGE_CHILD:
            return "child";
        case PAIR_STAGE_RECEIPT:
            return "receipt";
        case PAIR_STAGE_TIMEOUT:
            return "timeout";
    }
    return "child";
}

static bool pair_write_stdout(const char *line, size_t size)
{
    return line != NULL && size != 0U &&
        fwrite(line, 1U, size, stdout) == size && fflush(stdout) == 0;
}

static bool pair_emit_failure(PairFailureStage stage)
{
    char line[512U];
    const int size = snprintf(
        line,
        sizeof(line),
        "{\"schema\":\"DISPLAY_INGRESS_SIMULATED_PAIR_RECEIPT_V1\","
        "\"status\":\"FAILURE\",\"stage\":\"%s\","
        "\"fixture_only\":true,\"physical_evidence\":false,"
        "\"link_security\":false,\"clean_shutdown\":false}\n",
        pair_failure_stage_name(stage)
    );

    return size > 0 && (size_t)size < sizeof(line) &&
        pair_write_stdout(line, (size_t)size);
}

static bool pair_emit_success(
    const PairChild *host,
    const PairChild *edge
)
{
    char line[PAIR_AGGREGATE_CAPACITY];
    int size;

    if (host == NULL || edge == NULL || host->stdout_size < 2U ||
        edge->stdout_size < 2U ||
        host->stdout_bytes[host->stdout_size - 1U] != '\n' ||
        edge->stdout_bytes[edge->stdout_size - 1U] != '\n' ||
        host->stdout_size - 1U > (size_t)INT_MAX ||
        edge->stdout_size - 1U > (size_t)INT_MAX) {
        return false;
    }
    size = snprintf(
        line,
        sizeof(line),
        "{\"schema\":\"DISPLAY_INGRESS_SIMULATED_PAIR_RECEIPT_V1\","
        "\"status\":\"SUCCESS\",\"host\":%.*s,\"edge\":%.*s,"
        "\"fixture_only\":true,\"physical_evidence\":false,"
        "\"link_security\":false,\"clean_shutdown\":true}\n",
        (int)(host->stdout_size - 1U),
        host->stdout_bytes,
        (int)(edge->stdout_size - 1U),
        edge->stdout_bytes
    );
    return size > 0 && (size_t)size < sizeof(line) &&
        pair_write_stdout(line, (size_t)size);
}

static bool pair_exact_host_input_failure(const PairChild *host)
{
    static const char EXPECTED[] =
        "{\"schema\":\"DISPLAY_INGRESS_SIMULATED_SERVICE_RECEIPT_V2\","
        "\"role\":\"host\",\"status\":\"FAILURE\","
        "\"plane\":\"application\",\"stage\":\"input\","
        "\"carrier\":\"CONTROLLED_RAW_UDP\","
        "\"physical_evidence\":false,\"link_security\":false,"
        "\"clean_shutdown\":false}\n";

    return host != NULL && host->reaped && host->exit_code == 1 &&
        host->stderr_size == 0U &&
        host->stdout_size == sizeof(EXPECTED) - 1U &&
        memcmp(host->stdout_bytes, EXPECTED, sizeof(EXPECTED) - 1U) == 0;
}

int service_execute_pair(int frame_fd)
{
    int control[2] = {-1, -1};
    int media[2] = {-1, -1};
    int readiness[2] = {-1, -1};
    PairChild children[2U];
    PairRoleReceipt host_receipt;
    PairRoleReceipt edge_receipt;
    PairFailureStage failure_stage = PAIR_STAGE_BOOTSTRAP;
    PairReadyResult ready_result;
    PairCollectResult collect_result;
    const uint64_t start_ms = pair_now_ms();
    const uint64_t deadline_ms = start_ms == 0U ?
        0U : start_ms + PAIR_TIMEOUT_MS;
    size_t child_count = 0U;
    bool validated_success = false;

    pair_child_initialize(&children[0]);
    pair_child_initialize(&children[1]);
    if (!pair_prepare_signals()) {
        return 1;
    }
    if (deadline_ms == 0U) {
        goto done;
    }
    if (frame_fd < 3 || !pair_fd_is_open(frame_fd)) {
        failure_stage = PAIR_STAGE_INPUT;
        goto done;
    }
    if (!pair_make_tcp_pair(control) || !pair_make_udp_pair(media) ||
        !pair_make_pipe(readiness, true) ||
        !pair_spawn_role(
            MEISO_SESSION_ROLE_HOST,
            control[0],
            media[0],
            frame_fd,
            readiness[1],
            &children[0]
        )) {
        goto done;
    }
    child_count = 1U;
    pair_close_fd(&control[0]);
    pair_close_fd(&media[0]);
    pair_close_fd(&readiness[1]);
    pair_close_fd(&frame_fd);

    ready_result = pair_wait_for_host_ready(
        &children[0], &readiness[0], deadline_ms
    );
    if (ready_result != PAIR_READY_OK) {
        if (ready_result == PAIR_READY_TIMEOUT) {
            failure_stage = PAIR_STAGE_TIMEOUT;
        } else if (ready_result == PAIR_READY_EMPTY) {
            collect_result = pair_collect_children(
                children, child_count, deadline_ms
            );
            if (collect_result == PAIR_COLLECT_TIMEOUT) {
                failure_stage = PAIR_STAGE_TIMEOUT;
            } else if (collect_result == PAIR_COLLECT_OK &&
                       pair_exact_host_input_failure(&children[0])) {
                failure_stage = PAIR_STAGE_INPUT;
            } else {
                failure_stage = PAIR_STAGE_CHILD;
            }
        } else {
            failure_stage = ready_result == PAIR_READY_INVALID ?
                PAIR_STAGE_BOOTSTRAP : PAIR_STAGE_CHILD;
        }
        goto done;
    }

    if (!pair_spawn_role(
            MEISO_SESSION_ROLE_EDGE,
            control[1],
            media[1],
            -1,
            -1,
            &children[1]
        )) {
        failure_stage = PAIR_STAGE_BOOTSTRAP;
        goto done;
    }
    child_count = 2U;
    pair_close_fd(&control[1]);
    pair_close_fd(&media[1]);

    collect_result = pair_collect_children(children, child_count, deadline_ms);
    if (collect_result == PAIR_COLLECT_TIMEOUT) {
        failure_stage = PAIR_STAGE_TIMEOUT;
        goto done;
    }
    if (collect_result != PAIR_COLLECT_OK ||
        children[0].exit_code != 0 || children[1].exit_code != 0 ||
        children[0].stderr_size != 0U || children[1].stderr_size != 0U) {
        failure_stage = PAIR_STAGE_CHILD;
        goto done;
    }
    if (!pair_parse_role_receipt(&children[0], true, &host_receipt) ||
        !pair_parse_role_receipt(&children[1], false, &edge_receipt) ||
        !pair_role_receipts_match(&host_receipt, &edge_receipt)) {
        failure_stage = PAIR_STAGE_RECEIPT;
        goto done;
    }
    validated_success = true;

done:
    pair_close_fd(&frame_fd);
    pair_close_fd(&control[0]);
    pair_close_fd(&control[1]);
    pair_close_fd(&media[0]);
    pair_close_fd(&media[1]);
    pair_close_fd(&readiness[0]);
    pair_close_fd(&readiness[1]);
    pair_terminate_children(children, child_count);
    if (validated_success) {
        return pair_emit_success(&children[0], &children[1]) ? 0 : 1;
    }
    (void)pair_emit_failure(failure_stage);
    return 1;
}
